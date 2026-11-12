// Ride mode: the bike moves the player along a route through the level.
//
// When a level starts, the map is rasterised into a grid of 8-unit cells marking where the
// player fits, and A* finds a route from the player to the exit. Every tic RIDE_BuildTiccmd
// replaces the player's movement:
//
// - Speed comes from the bike's pace (cadence), so stopping pedalling stops the player.
// - The player faces the nearest visible monster (or the one the rider touched) and strafes so
//   the route is still followed; with no monster in sight the player faces along the route.
// - Doors and lifts on the route open as the player reaches them, keys or no keys. Floors that a
//   switch or trigger line moves (stairs, raising floors) are planned at the heights they can
//   reach, and the line that moves them is triggered when the player needs them.
// - Reaching the exit line or switch ends the level.
// - Holding a finger on a monster's sprite targets it and fires.
// - Boss levels (E1M8 and the like, which end when the bosses die rather than at an exit): ride
//   to the bosses, then circle the nearest at a distance, sidestepping their shots, until they
//   are dead. Then ride on to the level's end: an exit, or the sector that ends it (E1M8's
//   teleporter into a deadly room).
// - Damage the player deals is scaled by firepower, from pace and resistance (RIDE_ScaleDamage).
//
// Everything here runs on the game thread except the RIDE_Set* inputs, which the UI thread
// writes.

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "doom_android.h"
#include "ride.h"

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "info.h"
#include "m_fixed.h"
#include "p_local.h"
#include "p_spec.h"
#include "r_local.h"
#include "r_state.h"
#include "r_things.h"
#include "s_sound.h"
#include "sounds.h"
#include "tables.h"
#include "z_zone.h"

extern int viewwindowx, viewwindowy;

// ---------------------------------------------------------------------------
// Tuning

#define RIDE_TRACE      0       // 1: log position and progress once a second

#define CELL            8       // grid cell size, map units
#define CLEARANCE       15.0f   // half-size of the box kept clear of walls (the player's is 16)
#define MAX_STEP        24      // highest step the player climbs
#define PLAYER_HEIGHT   56
#define MAX_DROP_CELLS  3       // a drop off a ledge may skip this many blocked cells
#define HAZARD_COST     40      // extra cost per cell of damaging floor
#define LOOKAHEAD_CELLS 16      // steer towards a route cell up to this far ahead
#define DOOR_LOOKAHEAD  14      // open doors and lifts this many cells ahead
#define OFF_ROUTE       64.0f   // re-plan when this far from the route
#define REPLAN_TICS     (2*TICRATE)
#define STUCK_TICS      (2*TICRATE)
#define TARGET_RANGE    1024.0f
#define TARGET_TICS     4       // re-pick the target this often
#define TAPPED_LOST_TICS (2*TICRATE)
#define TOUCH_MARGIN    10      // pixels of slack around a sprite, in 320x200 screen units
#define FULL_PACE       4.5f    // metres per second for full speed (about 105 rpm)
#define MAX_MOVE        50      // DOOM's running forwardmove
#define EXIT_REACH      40.0f   // trigger the exit within this distance of its line
#define BOSS_APPROACH   160.0f  // ride until this close to a boss (it is fought once in sight)
#define BOSS_ENGAGE     1100.0f // fight a boss in sight this close
#define BOSS_UNSEEN_TICS (3*TICRATE) // keep fighting this long after losing sight of it
#define BOSS_KEEP_MIN   320.0f  // circle a boss between these distances
#define BOSS_KEEP_MAX   640.0f
#define DODGE_TICS      40      // react to a shot arriving within this many tics
#define DODGE_RADIUS    80.0f   // ... that would pass this close

// ---------------------------------------------------------------------------
// Inputs from the UI thread

static pthread_mutex_t g_inputLock = PTHREAD_MUTEX_INITIALIZER;
static int g_enabled = 1;
static int g_bikeLive;
static float g_pace;          // metres per second
static float g_resistance;    // 0..1
static int g_touchDown;
static float g_touchU, g_touchV;   // 0..1 across the 320x200 frame
static float g_firepower = 1.0f;   // written by the game thread, read by the UI

void RIDE_SetEnabled(int enabled)
{
    pthread_mutex_lock(&g_inputLock);
    g_enabled = enabled;
    pthread_mutex_unlock(&g_inputLock);
}

void RIDE_SetBike(int live, float pace, float resistance)
{
    pthread_mutex_lock(&g_inputLock);
    g_bikeLive = live;
    g_pace = pace < 0 ? 0 : pace;
    g_resistance = resistance < 0 ? 0 : resistance > 1 ? 1 : resistance;
    pthread_mutex_unlock(&g_inputLock);
}

void RIDE_SetTouch(int down, float u, float v)
{
    pthread_mutex_lock(&g_inputLock);
    g_touchDown = down;
    g_touchU = u;
    g_touchV = v;
    pthread_mutex_unlock(&g_inputLock);
}

// ---------------------------------------------------------------------------
// Level state

enum { ROUTE_NONE, ROUTE_OK, ROUTE_FAILED };

static int g_gridW, g_gridH;
static int g_originX, g_originY;     // map coordinates of cell (0,0)'s corner
static byte* g_cellFlags;            // CF_* per cell
static short* g_cellSector;          // sector index per cell
static int* g_secFlags;              // SF_* per sector
static fixed_t* g_secLowFloor;       // lowest the floor can go (lifts, lowering floors)
static fixed_t* g_secHighFloor;      // highest the floor can go (stairs, raising floors)
#define MAX_MOVERS 4
static int (*g_secMovers)[MAX_MOVERS]; // lines whose action moves the sector's floor, or -1
static byte* g_lineTriggered;        // per line: already triggered by the ride
static fixed_t* g_secOpenCeiling;    // highest the ceiling can go (doors)

#define CF_WALL    1   // too close to a wall
#define CF_LEDGE   2   // too close to a ledge that is too high to climb (passable when dropping)
#define CF_GOAL    4
#define CF_HAZARD  8
#define CF_TELE    16  // a teleporter line starts here
#define CF_BOSS    32  // near where a boss started: the approach route's goal

#define SF_LIFT    1
#define SF_HAZARD  2

typedef struct
{
    int fromCell;
    int toCell;
} teleedge_t;

static teleedge_t* g_tele;
static int g_numTele;

static line_t* g_exitLines[16];
static int g_numExitLines;
static int g_numGoalCells;   // cells marked CF_GOAL: near an exit line, or in a level-ending sector

// Boss levels
static int g_bossTypes[2];   // mobjtype_t that must die to end the level, -1 for none
static float g_bossX[8], g_bossY[8];   // where live bosses were when the grid was built
static int g_numBossSpots;
static int g_bossCleared;    // bosses dead and their floors lowered: the grid has been rebuilt
static int g_orbit = 1;      // which way round the boss: 1 or -1
static int g_orbitBlocked;
static int g_bossUnseen;
static int g_bossDeadTic = -1;   // when the last boss was first seen dead
static int g_bossFloorMoved;     // a tag 666/667 sector has started moving since

static int* g_route;         // cell indices from start to goal
static int g_routeLen;
static int g_routeState;
static int g_progress;       // index into g_route of the cell nearest the player
static int g_lastPlanTic = -1000;
static int g_level;          // bumped by RIDE_LevelLoaded so the game thread rebuilds

static int g_builtLevel = -1;
static int g_exited;         // the exit has been triggered this level
static mobj_t* g_target;
static int g_targetTapped;
static int g_targetUnseen;
static int g_stuckTics;
static int g_nudgeSign = 1;  // which way to sidestep something blocking the route
static fixed_t g_stuckX, g_stuckY;

static int CellIndex(int cx, int cy) { return cy * g_gridW + cx; }
static float CellX(int c) { return g_originX + (c % g_gridW) * CELL + CELL / 2.0f; }
static float CellY(int c) { return g_originY + (c / g_gridW) * CELL + CELL / 2.0f; }

static int CellAt(float x, float y)
{
    int cx = (int)floorf((x - g_originX) / CELL);
    int cy = (int)floorf((y - g_originY) / CELL);
    if (cx < 0 || cy < 0 || cx >= g_gridW || cy >= g_gridH)
        return -1;
    return CellIndex(cx, cy);
}

static int Walkable(int c) { return c >= 0 && !(g_cellFlags[c] & (CF_WALL | CF_LEDGE)); }

static float Dist2Cell(int c, float x, float y)
{
    float dx = CellX(c) - x, dy = CellY(c) - y;
    return dx * dx + dy * dy;
}

static void FreeLevel(void)
{
    free(g_cellFlags); g_cellFlags = NULL;
    free(g_cellSector); g_cellSector = NULL;
    free(g_secFlags); g_secFlags = NULL;
    free(g_secLowFloor); g_secLowFloor = NULL;
    free(g_secHighFloor); g_secHighFloor = NULL;
    free(g_secMovers); g_secMovers = NULL;
    free(g_lineTriggered); g_lineTriggered = NULL;
    free(g_secOpenCeiling); g_secOpenCeiling = NULL;
    free(g_tele); g_tele = NULL;
    free(g_route); g_route = NULL;
    g_numTele = g_routeLen = g_numExitLines = g_numGoalCells = g_numBossSpots = 0;
    g_routeState = ROUTE_NONE;
    g_target = NULL;
    g_exited = 0;
}

// The monsters whose deaths end this level (see A_BossDeath), if it is a boss level.
static void FindBossTypes(void)
{
    g_bossTypes[0] = g_bossTypes[1] = -1;
    if (gamemode == commercial)
    {
        if (gamemap == 7)
        {
            g_bossTypes[0] = MT_FATSO;
            g_bossTypes[1] = MT_BABY;
        }
        return;
    }
    if (gameepisode == 1 && gamemap == 8)
        g_bossTypes[0] = MT_BRUISER;
    else if ((gameepisode == 2 && gamemap == 8) || (gameepisode == 4 && gamemap == 6))
        g_bossTypes[0] = MT_CYBORG;
    else if ((gameepisode == 3 || gameepisode == 4) && gamemap == 8)
        g_bossTypes[0] = MT_SPIDER;
}

static int IsBoss(mobj_t* m)
{
    return m->health > 0 && ((int)m->type == g_bossTypes[0] || (int)m->type == g_bossTypes[1]);
}

static mobj_t* NearestBoss(mobj_t* pmo, float* distOut)
{
    mobj_t* best = NULL;
    float bestD = 1e18f;
    if (g_bossTypes[0] < 0)
        return NULL;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker || !IsBoss((mobj_t*)th))
            continue;
        mobj_t* m = (mobj_t*)th;
        float dx = (m->x - pmo->x) / (float)FRACUNIT, dy = (m->y - pmo->y) / (float)FRACUNIT;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = m; }
    }
    if (distOut)
        *distOut = best ? sqrtf(bestD) : 0;
    return best;
}

// After the last boss dies, A_BossDeath (at the end of its death animation) may move tag 666
// or 667 sectors: wait until one has moved and they have all stopped, or 10 seconds.
static int BossFloorsSettled(void)
{
    int moving = 0, tagged = 0;
    if (g_bossDeadTic < 0)
        g_bossDeadTic = gametic;
    for (int s = 0; s < numsectors; s++)
        if (sectors[s].tag == 666 || sectors[s].tag == 667)
        {
            tagged = 1;
            if (sectors[s].specialdata)
                moving = 1;
        }
    if (moving)
        g_bossFloorMoved = 1;
    if (!tagged)
        return 1;
    return (g_bossFloorMoved && !moving) || gametic - g_bossDeadTic > 10 * TICRATE;
}

void RIDE_LevelLoaded(void)
{
    // Called from P_SetupLevel; the grid is built on the first tic of the level instead,
    // once the player's mobj exists.
    g_level++;
    g_bossCleared = 0;
    g_bossDeadTic = -1;
    g_bossFloorMoved = 0;
}

// ---------------------------------------------------------------------------
// Grid construction

static float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrtf(qx * qx + qy * qy);
}

// 1 if (x,y) is on the line's front (right) side, as P_PointOnLineSide's 0.
static int OnFront(line_t* l, float x, float y)
{
    float ax = l->v1->x / (float)FRACUNIT, ay = l->v1->y / (float)FRACUNIT;
    float dx = l->dx / (float)FRACUNIT, dy = l->dy / (float)FRACUNIT;
    return (x - ax) * dy - (y - ay) * dx > 0;
}

// Does the segment touch the square of half-size h centred on (cx,cy)? The player is an
// axis-aligned box, so near a diagonal wall its corner reaches further than its "radius".
static int SegmentHitsBox(float cx, float cy, float h, float ax, float ay, float bx, float by)
{
    float t0 = 0, t1 = 1;
    float d[2] = { bx - ax, by - ay };
    float lo[2] = { cx - h - ax, cy - h - ay };
    float hi[2] = { cx + h - ax, cy + h - ay };
    for (int k = 0; k < 2; k++)
    {
        if (fabsf(d[k]) < 1e-6f)
        {
            if (lo[k] > 0 || hi[k] < 0)
                return 0;
            continue;
        }
        float ta = lo[k] / d[k], tb = hi[k] / d[k];
        if (ta > tb) { float t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1)
            return 0;
    }
    return 1;
}

// Marks every cell whose player-sized box (radius = half-size) the line touches with flag;
// side: -1 both, 1 front only, 0 back only.
static void MarkNearLine(line_t* l, float radius, int flag, int side)
{
    float ax = l->v1->x / (float)FRACUNIT, ay = l->v1->y / (float)FRACUNIT;
    float bx = l->v2->x / (float)FRACUNIT, by = l->v2->y / (float)FRACUNIT;
    int x0 = (int)floorf((fminf(ax, bx) - radius - g_originX) / CELL);
    int x1 = (int)floorf((fmaxf(ax, bx) + radius - g_originX) / CELL);
    int y0 = (int)floorf((fminf(ay, by) - radius - g_originY) / CELL);
    int y1 = (int)floorf((fmaxf(ay, by) + radius - g_originY) / CELL);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g_gridW) x1 = g_gridW - 1;
    if (y1 >= g_gridH) y1 = g_gridH - 1;

    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
        {
            int c = CellIndex(cx, cy);
            float px = CellX(c), py = CellY(c);
            if (!SegmentHitsBox(px, py, radius, ax, ay, bx, by))
                continue;
            if (side >= 0 && OnFront(l, px, py) != side)
                continue;
            g_cellFlags[c] |= flag;
        }
}

static int IsLiftSpecial(int special)
{
    return special == 10 || special == 21 || special == 62 || special == 88 ||
           (special >= 120 && special <= 123);
}

static int StairStep(int special)
{
    switch (special)
    {
      case 7: case 8: return 8;       // S1, W1 build stairs
      case 100: case 127: return 16;  // W1, S1 turbo stairs
    }
    return 0;
}

// Specials that move the floors of the sectors they tag (lifts are handled on their own).
static int IsFloorMoverSpecial(int special)
{
    switch (special)
    {
      case 5: case 9: case 14: case 15: case 18: case 19: case 20: case 22: case 23: case 24:
      case 30: case 36: case 37: case 38: case 45: case 47: case 55: case 56: case 58: case 59:
      case 60: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 82:
      case 83: case 84: case 91: case 92: case 93: case 94: case 95: case 96: case 98: case 101:
      case 102: case 119: case 128: case 129: case 130: case 131: case 132: case 140:
        return 1;
    }
    return 0;
}

static int IsWalkSpecial(int special)
{
    switch (special)
    {
      case 2: case 3: case 4: case 5: case 6: case 8: case 10: case 12: case 13: case 16: case 17:
      case 19: case 22: case 25: case 30: case 35: case 36: case 37: case 38: case 39: case 40:
      case 44: case 52: case 53: case 54: case 56: case 57: case 58: case 59: case 72: case 73:
      case 74: case 75: case 76: case 77: case 79: case 80: case 81: case 82: case 83: case 84:
      case 86: case 87: case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95:
      case 96: case 97: case 98: case 100: case 104: case 105: case 106: case 107: case 108:
      case 109: case 110: case 119: case 120: case 121: case 124: case 125: case 126: case 128:
      case 129: case 130: case 141:
        return 1;
    }
    return 0;
}

static int IsGunSpecial(int special)
{
    return special == 24 || special == 46 || special == 47;
}

static void AddMover(int s, int line)
{
    for (int k = 0; k < MAX_MOVERS; k++)
    {
        if (g_secMovers[s][k] == line)
            return;
        if (g_secMovers[s][k] < 0)
        {
            g_secMovers[s][k] = line;
            return;
        }
    }
}

// The heights a staircase line builds, following EV_BuildStairs.
static void PlanStairs(int lineIndex)
{
    line_t* line = &lines[lineIndex];
    fixed_t stepSize = StairStep(line->special) * FRACUNIT;
    for (int s0 = 0; s0 < numsectors; s0++)
    {
        if (sectors[s0].tag != line->tag)
            continue;
        sector_t* sec = &sectors[s0];
        fixed_t height = sec->floorheight + stepSize;
        int texture = sec->floorpic;
        int guard = 0;
        for (;;)
        {
            int s = sec - sectors;
            AddMover(s, lineIndex);
            if (height > g_secHighFloor[s])
                g_secHighFloor[s] = height;

            sector_t* next = NULL;
            for (int i = 0; i < sec->linecount && !next; i++)
            {
                line_t* l = sec->lines[i];
                if (!(l->flags & ML_TWOSIDED) || l->frontsector != sec || l->backsector->floorpic != texture)
                    continue;
                next = l->backsector;
            }
            if (!next || ++guard > 256)
                break;
            height += stepSize;
            sec = next;
        }
    }
}

static int IsExitSpecial(int special, int secret)
{
    return secret ? (special == 51 || special == 124) : (special == 11 || special == 52);
}

static void ComputeSectorLimits(void)
{
    g_secFlags = calloc(numsectors, sizeof(int));
    g_secLowFloor = malloc(numsectors * sizeof(fixed_t));
    g_secHighFloor = malloc(numsectors * sizeof(fixed_t));
    g_secOpenCeiling = malloc(numsectors * sizeof(fixed_t));
    g_secMovers = malloc(numsectors * sizeof(*g_secMovers));
    memset(g_secMovers, 0xff, numsectors * sizeof(*g_secMovers));
    g_lineTriggered = calloc(numlines, 1);
    for (int s = 0; s < numsectors; s++)
        g_secLowFloor[s] = g_secHighFloor[s] = sectors[s].floorheight;

    // Sectors tagged by a lift line can be lowered.
    for (int i = 0; i < numlines; i++)
        if (IsLiftSpecial(lines[i].special) && lines[i].tag)
            for (int s = 0; s < numsectors; s++)
                if (sectors[s].tag == lines[i].tag)
                    g_secFlags[s] |= SF_LIFT;

    // Floors moved by stairs and other floor lines: anywhere between the lowest and highest
    // neighbouring floors (stairs: the height they are built to).
    for (int i = 0; i < numlines; i++)
    {
        line_t* l = &lines[i];
        if (!l->tag)
            continue;
        if (StairStep(l->special))
            PlanStairs(i);
        else if (IsFloorMoverSpecial(l->special))
            for (int s = 0; s < numsectors; s++)
            {
                if (sectors[s].tag != l->tag)
                    continue;
                AddMover(s, i);
                fixed_t low = P_FindLowestFloorSurrounding(&sectors[s]);
                fixed_t high = P_FindHighestFloorSurrounding(&sectors[s]);
                if (low < g_secLowFloor[s]) g_secLowFloor[s] = low;
                if (high > g_secHighFloor[s]) g_secHighFloor[s] = high;
            }
    }

    for (int s = 0; s < numsectors; s++)
    {
        sector_t* sec = &sectors[s];
        if (g_secFlags[s] & SF_LIFT)
        {
            fixed_t low = P_FindLowestFloorSurrounding(sec);
            if (low < g_secLowFloor[s])
                g_secLowFloor[s] = low;
        }

        // Any sector too low to walk through is treated as a door that opens to just below
        // its lowest neighbouring ceiling, as DOOM's doors do.
        g_secOpenCeiling[s] = sec->ceilingheight;
        // Not a boss level's walls (tag 666, 667): only the bosses' deaths open those.
        if (sec->ceilingheight - sec->floorheight < PLAYER_HEIGHT * FRACUNIT &&
            sec->tag != 666 && sec->tag != 667)
        {
            fixed_t top = P_FindLowestCeilingSurrounding(sec);
            if (top != MAXINT && top - 4 * FRACUNIT > g_secOpenCeiling[s])
                g_secOpenCeiling[s] = top - 4 * FRACUNIT;
        }

        switch (sec->special)
        {
          case 4: case 5: case 7: case 11: case 16:
            g_secFlags[s] |= SF_HAZARD;
            break;
        }
    }
}

static int SectorIndex(sector_t* s) { return s - sectors; }

// Boss levels: the approach route ends near where the bosses are now.
static void MarkBossSpots(void)
{
    int cells = g_gridW * g_gridH;
    for (int c = 0; c < cells; c++)
        g_cellFlags[c] &= ~CF_BOSS;
    g_numBossSpots = 0;
    for (thinker_t* th = thinkercap.next; th != &thinkercap && g_numBossSpots < 8; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker || !IsBoss((mobj_t*)th))
            continue;
        mobj_t* m = (mobj_t*)th;
        g_bossX[g_numBossSpots] = m->x / (float)FRACUNIT;
        g_bossY[g_numBossSpots] = m->y / (float)FRACUNIT;
        g_numBossSpots++;
    }
    for (int b = 0; b < g_numBossSpots; b++)
    {
        int c0 = CellAt(g_bossX[b] - BOSS_APPROACH, g_bossY[b] - BOSS_APPROACH);
        int c1 = CellAt(g_bossX[b] + BOSS_APPROACH, g_bossY[b] + BOSS_APPROACH);
        int x0 = c0 < 0 ? 0 : c0 % g_gridW, y0 = c0 < 0 ? 0 : c0 / g_gridW;
        int x1 = c1 < 0 ? g_gridW - 1 : c1 % g_gridW, y1 = c1 < 0 ? g_gridH - 1 : c1 / g_gridW;
        for (int cy = y0; cy <= y1; cy++)
            for (int cx = x0; cx <= x1; cx++)
            {
                int c = CellIndex(cx, cy);
                if (Dist2Cell(c, g_bossX[b], g_bossY[b]) < BOSS_APPROACH * BOSS_APPROACH)
                    g_cellFlags[c] |= CF_BOSS;
            }
    }
}

static void BuildGrid(void)
{
    fixed_t minX = MAXINT, minY = MAXINT, maxX = MININT, maxY = MININT;
    for (int i = 0; i < numvertexes; i++)
    {
        if (vertexes[i].x < minX) minX = vertexes[i].x;
        if (vertexes[i].x > maxX) maxX = vertexes[i].x;
        if (vertexes[i].y < minY) minY = vertexes[i].y;
        if (vertexes[i].y > maxY) maxY = vertexes[i].y;
    }
    g_originX = (minX >> FRACBITS) - 2 * CELL;
    g_originY = (minY >> FRACBITS) - 2 * CELL;
    g_gridW = ((maxX - minX) >> FRACBITS) / CELL + 5;
    g_gridH = ((maxY - minY) >> FRACBITS) / CELL + 5;

    int cells = g_gridW * g_gridH;
    g_cellFlags = calloc(cells, 1);
    g_cellSector = malloc(cells * sizeof(short));

    ComputeSectorLimits();

    for (int c = 0; c < cells; c++)
    {
        sector_t* sec = R_PointInSubsector((fixed_t)(CellX(c) * FRACUNIT), (fixed_t)(CellY(c) * FRACUNIT))->sector;
        int s = SectorIndex(sec);
        g_cellSector[c] = (short)s;
        if (g_secFlags[s] & SF_HAZARD)
            g_cellFlags[c] |= CF_HAZARD;
    }

    for (int i = 0; i < numlines; i++)
    {
        line_t* l = &lines[i];
        if (!l->backsector || (l->flags & ML_BLOCKING))
        {
            MarkNearLine(l, CLEARANCE, CF_WALL, -1);
            continue;
        }

        int f = SectorIndex(l->frontsector), b = SectorIndex(l->backsector);
        fixed_t loF = g_secLowFloor[f], loB = g_secLowFloor[b];
        fixed_t hiF = g_secHighFloor[f], hiB = g_secHighFloor[b];
        fixed_t ceil = g_secOpenCeiling[f] < g_secOpenCeiling[b] ? g_secOpenCeiling[f] : g_secOpenCeiling[b];
        fixed_t floor = loF > loB ? loF : loB;

        if (ceil - floor < PLAYER_HEIGHT * FRACUNIT)
            MarkNearLine(l, CLEARANCE, CF_WALL, -1);       // never enough headroom
        else
        {
            // Too high to climb from the low side; the high side can still drop down.
            if (loB - hiF > MAX_STEP * FRACUNIT)
                MarkNearLine(l, CLEARANCE, CF_LEDGE, 1);
            if (loF - hiB > MAX_STEP * FRACUNIT)
                MarkNearLine(l, CLEARANCE, CF_LEDGE, 0);
        }
    }

    // Solid scenery (pillars, lamps, barrels) blocks like a wall. Monsters move, so they don't.
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* mo = (mobj_t*)th;
        if (!(mo->flags & MF_SOLID) || (mo->flags & MF_COUNTKILL) || mo->player)
            continue;
        float r = mo->radius / (float)FRACUNIT + CLEARANCE;
        float mx = mo->x / (float)FRACUNIT, my = mo->y / (float)FRACUNIT;
        int c0 = CellAt(mx - r, my - r), c1 = CellAt(mx + r, my + r);
        if (c0 < 0 || c1 < 0)
            continue;
        for (int cy = c0 / g_gridW; cy <= c1 / g_gridW; cy++)
            for (int cx = c0 % g_gridW; cx <= c1 % g_gridW; cx++)
            {
                int c = CellIndex(cx, cy);
                if (fabsf(CellX(c) - mx) < r && fabsf(CellY(c) - my) < r)
                    g_cellFlags[c] |= CF_WALL;
            }
    }

    // Exits: prefer the normal exit, fall back to the secret one.
    for (int secret = 0; secret <= 1 && !g_numExitLines; secret++)
        for (int i = 0; i < numlines && g_numExitLines < 16; i++)
            if (IsExitSpecial(lines[i].special, secret))
                g_exitLines[g_numExitLines++] = &lines[i];
    for (int i = 0; i < g_numExitLines; i++)
        MarkNearLine(g_exitLines[i], EXIT_REACH - 8, CF_GOAL, -1);
    if (!g_numExitLines)
    {
        // No exit line (E1M8): the level ends in a sector with special 11.
        for (int c = 0; c < cells; c++)
            if (sectors[g_cellSector[c]].special == 11)
                g_cellFlags[c] |= CF_GOAL;
    }
    for (int c = 0; c < cells; c++)
        if ((g_cellFlags[c] & CF_GOAL) && Walkable(c))
            g_numGoalCells++;

    FindBossTypes();
    MarkBossSpots();

    // Teleporters (W1 39, WR 97): an edge from the front of the line to its destination.
    g_tele = malloc(sizeof(teleedge_t) * 4096);
    for (int i = 0; i < numlines; i++)
    {
        line_t* l = &lines[i];
        if (l->special != 39 && l->special != 97)
            continue;
        mobj_t* dest = NULL;
        for (thinker_t* th = thinkercap.next; th != &thinkercap && !dest; th = th->next)
        {
            if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                continue;
            mobj_t* mo = (mobj_t*)th;
            if (mo->type == MT_TELEPORTMAN && mo->subsector->sector->tag == l->tag)
                dest = mo;
        }
        if (!dest)
            continue;
        int to = CellAt(dest->x / (float)FRACUNIT, dest->y / (float)FRACUNIT);
        if (to < 0)
            continue;

        float ax = l->v1->x / (float)FRACUNIT, ay = l->v1->y / (float)FRACUNIT;
        float bx = l->v2->x / (float)FRACUNIT, by = l->v2->y / (float)FRACUNIT;
        int c0 = CellAt(fminf(ax, bx) - 16, fminf(ay, by) - 16);
        int c1 = CellAt(fmaxf(ax, bx) + 16, fmaxf(ay, by) + 16);
        if (c0 < 0 || c1 < 0)
            continue;
        for (int cy = c0 / g_gridW; cy <= c1 / g_gridW; cy++)
            for (int cx = c0 % g_gridW; cx <= c1 % g_gridW; cx++)
            {
                int c = CellIndex(cx, cy);
                if (g_numTele >= 4096 || !Walkable(c))
                    continue;
                if (SegmentDistance(CellX(c), CellY(c), ax, ay, bx, by) > 12 || !OnFront(l, CellX(c), CellY(c)))
                    continue;
                g_tele[g_numTele].fromCell = c;
                g_tele[g_numTele].toCell = to;
                g_numTele++;
                g_cellFlags[c] |= CF_TELE;
            }
    }

    LOGI("Ride: %dx%d grid, %d exit lines, %d goal cells, %d teleporter cells, %d bosses",
         g_gridW, g_gridH, g_numExitLines, g_numGoalCells, g_numTele, g_numBossSpots);
}

// ---------------------------------------------------------------------------
// Route: A* over the grid

static int StepAllowed(int from, int to)
{
    int a = g_cellSector[from], b = g_cellSector[to];
    if (a == b)
        return 1;
    fixed_t hiA = g_secHighFloor[a];
    if (g_secLowFloor[b] - hiA > MAX_STEP * FRACUNIT)
        return 0;
    fixed_t ceil = g_secOpenCeiling[a] < g_secOpenCeiling[b] ? g_secOpenCeiling[a] : g_secOpenCeiling[b];
    fixed_t floor = g_secLowFloor[a] > g_secLowFloor[b] ? g_secLowFloor[a] : g_secLowFloor[b];
    return ceil - floor >= PLAYER_HEIGHT * FRACUNIT;
}

typedef struct { int f; int cell; } heapnode_t;
static heapnode_t* g_heap;
static int g_heapSize, g_heapCap;

static void HeapPush(int f, int cell)
{
    if (g_heapSize == g_heapCap)
    {
        g_heapCap = g_heapCap ? g_heapCap * 2 : 4096;
        g_heap = realloc(g_heap, g_heapCap * sizeof(heapnode_t));
    }
    int i = g_heapSize++;
    while (i > 0)
    {
        int p = (i - 1) / 2;
        if (g_heap[p].f <= f)
            break;
        g_heap[i] = g_heap[p];
        i = p;
    }
    g_heap[i].f = f;
    g_heap[i].cell = cell;
}

static heapnode_t HeapPop(void)
{
    heapnode_t top = g_heap[0];
    heapnode_t last = g_heap[--g_heapSize];
    int i = 0;
    for (;;)
    {
        int c = 2 * i + 1;
        if (c >= g_heapSize)
            break;
        if (c + 1 < g_heapSize && g_heap[c + 1].f < g_heap[c].f)
            c++;
        if (g_heap[c].f >= last.f)
            break;
        g_heap[i] = g_heap[c];
        i = c;
    }
    g_heap[i] = last;
    return top;
}

static int g_goalFlag = CF_GOAL;   // what PlanRoute is heading for

static int Heuristic(int c)
{
    float best = 1e9f, x = CellX(c), y = CellY(c);
    if (g_goalFlag == CF_BOSS)
    {
        for (int b = 0; b < g_numBossSpots; b++)
        {
            float d = sqrtf(Dist2Cell(c, g_bossX[b], g_bossY[b])) - BOSS_APPROACH;
            if (d < best)
                best = d;
        }
        return best < 0 ? 0 : (int)(best / CELL * 10);
    }
    if (!g_numExitLines)
        return 0;   // goal sectors: plain Dijkstra
    for (int i = 0; i < g_numExitLines; i++)
    {
        line_t* l = g_exitLines[i];
        float d = SegmentDistance(x, y, l->v1->x / (float)FRACUNIT, l->v1->y / (float)FRACUNIT,
                                  l->v2->x / (float)FRACUNIT, l->v2->y / (float)FRACUNIT) - EXIT_REACH;
        if (d < best)
            best = d;
    }
    if (best < 0)
        best = 0;
    return (int)(best / CELL * 10);
}

static int NearestWalkable(float x, float y)
{
    int c = CellAt(x, y);
    if (Walkable(c))
        return c;
    int best = -1;
    float bestD = 1e9f;
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
        {
            int n = CellAt(x + dx * CELL, y + dy * CELL);
            if (!Walkable(n))
                continue;
            float d = (CellX(n) - x) * (CellX(n) - x) + (CellY(n) - y) * (CellY(n) - y);
            if (d < bestD) { bestD = d; best = n; }
        }
    return best;
}

static void PlanRoute(mobj_t* mo, int goalFlag)
{
    static const int dirs[8][3] = {
        { 1, 0, 10 }, { -1, 0, 10 }, { 0, 1, 10 }, { 0, -1, 10 },
        { 1, 1, 14 }, { 1, -1, 14 }, { -1, 1, 14 }, { -1, -1, 14 } };

    g_lastPlanTic = gametic;
    free(g_route);
    g_route = NULL;
    g_routeLen = 0;
    g_progress = 0;
    g_routeState = ROUTE_FAILED;

    int start = NearestWalkable(mo->x / (float)FRACUNIT, mo->y / (float)FRACUNIT);
    g_goalFlag = goalFlag;
    if (start < 0 || (goalFlag == CF_GOAL && !g_numGoalCells) || (goalFlag == CF_BOSS && !g_numBossSpots))
    {
        LOGI("Ride: no route (%s)", start < 0 ? "player off the grid" : "nowhere to go");
        return;
    }

    int cells = g_gridW * g_gridH;
    int* cost = malloc(cells * sizeof(int));
    int* parent = malloc(cells * sizeof(int));
    byte* closed = calloc(cells, 1);
    for (int i = 0; i < cells; i++)
        cost[i] = MAXINT;

    g_heapSize = 0;
    cost[start] = 0;
    parent[start] = -1;
    HeapPush(Heuristic(start), start);
    int goal = -1;

    while (g_heapSize)
    {
        int c = HeapPop().cell;
        if (closed[c])
            continue;
        closed[c] = 1;
        if (g_cellFlags[c] & goalFlag)
        {
            goal = c;
            break;
        }
        int cx = c % g_gridW, cy = c / g_gridW;

        for (int d = 0; d < 8; d++)
        {
            // Walk up to MAX_DROP_CELLS: a ledge cell in between is only crossable going down.
            int n = -1, steps = 0;
            for (int k = 1; k <= MAX_DROP_CELLS; k++)
            {
                int nx = cx + dirs[d][0] * k, ny = cy + dirs[d][1] * k;
                if (nx < 0 || ny < 0 || nx >= g_gridW || ny >= g_gridH)
                    break;
                int m = CellIndex(nx, ny);
                if (g_cellFlags[m] & CF_WALL)
                    break;
                if (Walkable(m))
                {
                    if (k == 1 || sectors[g_cellSector[m]].floorheight < sectors[g_cellSector[c]].floorheight)
                    {
                        n = m;
                        steps = k;
                    }
                    break;
                }
            }
            if (n < 0 || closed[n] || !StepAllowed(c, n))
                continue;
            if (d >= 4)
            {
                // No cutting corners past walls.
                if (!Walkable(CellIndex(cx + dirs[d][0], cy)) || !Walkable(CellIndex(cx, cy + dirs[d][1])))
                    if (steps == 1)
                        continue;
            }
            int g = cost[c] + dirs[d][2] * steps + ((g_cellFlags[n] & CF_HAZARD) ? HAZARD_COST : 0);
            if (g < cost[n])
            {
                cost[n] = g;
                parent[n] = c;
                HeapPush(g + Heuristic(n), n);
            }
        }

        if (g_cellFlags[c] & CF_TELE)
            for (int t = 0; t < g_numTele; t++)
            {
                if (g_tele[t].fromCell != c)
                    continue;
                int n = g_tele[t].toCell;
                int g = cost[c] + 10;
                if (!closed[n] && g < cost[n])
                {
                    cost[n] = g;
                    parent[n] = c;
                    HeapPush(g + Heuristic(n), n);
                }
            }
    }

    if (goal >= 0)
    {
        int len = 0;
        for (int c = goal; c != -1; c = parent[c])
            len++;
        g_route = malloc(len * sizeof(int));
        g_routeLen = len;
        for (int c = goal, i = len - 1; c != -1; c = parent[c], i--)
            g_route[i] = c;
        g_routeState = ROUTE_OK;
        LOGI("Ride: route of %d cells (%d units) to the %s", len, cost[goal] * CELL / 10,
             goalFlag == CF_BOSS ? "bosses" : "end of the level");
    }
    else
    {
        // Say where the search got closest to the exit, to see what blocks the way.
        int nearest = start, bestH = MAXINT, reached = 0;
        for (int c = 0; c < cells; c++)
            if (closed[c])
            {
                reached++;
                int h = Heuristic(c);
                if (h < bestH) { bestH = h; nearest = c; }
            }
        sector_t* s = &sectors[g_cellSector[nearest]];
        LOGI("Ride: no route to the exit; %d cells reached, nearest (%.0f,%.0f) sector %d "
             "floor %d ceiling %d, %d units from the exit",
             reached, CellX(nearest), CellY(nearest), g_cellSector[nearest],
             s->floorheight >> FRACBITS, s->ceilingheight >> FRACBITS, bestH * CELL / 10);

        // Lines between a reached and an unreached sector, nearest the exit first.
        byte* reachedSec = calloc(numsectors, 1);
        for (int c = 0; c < cells; c++)
            if (closed[c])
                reachedSec[g_cellSector[c]] = 1;
        for (int pass = 0; pass < 12; pass++)
        {
            int bestLine = -1, bestD = MAXINT;
            for (int i = 0; i < numlines; i++)
            {
                line_t* l = &lines[i];
                if (!l->backsector || l->validcount == -7)
                    continue;
                int f = SectorIndex(l->frontsector), b = SectorIndex(l->backsector);
                if (reachedSec[f] == reachedSec[b])
                    continue;
                int d = Heuristic(CellAt((l->v1->x + l->v2->x) / 2 / (float)FRACUNIT, (l->v1->y + l->v2->y) / 2 / (float)FRACUNIT));
                if (d < bestD) { bestD = d; bestLine = i; }
            }
            if (bestLine < 0)
                break;
            line_t* l = &lines[bestLine];
            l->validcount = -7;
            int f = SectorIndex(l->frontsector), b = SectorIndex(l->backsector);
            LOGI("Ride:   line %d special %d flags %d: sector %d%s floor %d..%d ceil %d | sector %d%s floor %d..%d ceil %d",
                 bestLine, l->special, l->flags,
                 f, reachedSec[f] ? "*" : "", g_secLowFloor[f] >> FRACBITS, sectors[f].floorheight >> FRACBITS, g_secOpenCeiling[f] >> FRACBITS,
                 b, reachedSec[b] ? "*" : "", g_secLowFloor[b] >> FRACBITS, sectors[b].floorheight >> FRACBITS, g_secOpenCeiling[b] >> FRACBITS);
        }
        for (int i = 0; i < numlines; i++)
            if (lines[i].validcount == -7)
                lines[i].validcount = 0;
        free(reachedSec);
    }

    free(cost);
    free(parent);
    free(closed);
}

// ---------------------------------------------------------------------------
// Following the route

static float Dist2(float ax, float ay, float bx, float by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

static int IsTeleportHop(int i)
{
    // Consecutive route cells more than a drop apart were joined by a teleporter.
    return i + 1 < g_routeLen && Dist2(CellX(g_route[i]), CellY(g_route[i]),
                                       CellX(g_route[i + 1]), CellY(g_route[i + 1])) > (float)(CELL * 5) * (CELL * 5);
}

// Straight walk across walkable cells from (x0,y0) to (x1,y1)?
static int ClearPath(float x0, float y0, float x1, float y1)
{
    float d = sqrtf(Dist2(x0, y0, x1, y1));
    int steps = (int)(d / (CELL / 2)) + 1;
    int prev = CellAt(x0, y0);
    for (int i = 1; i <= steps; i++)
    {
        float t = (float)i / steps;
        int c = CellAt(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
        if (!Walkable(c) || (prev >= 0 && c != prev && !StepAllowed(prev, c)))
            return 0;
        prev = c;
    }
    return 1;
}

static void UpdateProgress(float x, float y)
{
    int lo = g_progress - 8, hi = g_progress + 40;
    if (lo < 0) lo = 0;
    if (hi > g_routeLen) hi = g_routeLen;
    float best = 1e18f;
    int bestI = g_progress;
    for (int i = lo; i < hi; i++)
    {
        float d = Dist2(x, y, CellX(g_route[i]), CellY(g_route[i]));
        if (d < best) { best = d; bestI = i; }
    }
    g_progress = bestI;
    if (best > OFF_ROUTE * OFF_ROUTE && gametic - g_lastPlanTic > REPLAN_TICS / 4)
        g_routeState = ROUTE_NONE;   // teleported, pushed or loaded elsewhere: plan again
}

// The point to steer towards: the farthest route cell ahead in a straight line.
static void Lookahead(float x, float y, float* tx, float* ty)
{
    int best = g_progress + 1 < g_routeLen ? g_progress + 1 : g_progress;
    for (int i = g_progress + 1; i < g_routeLen && i <= g_progress + LOOKAHEAD_CELLS; i++)
    {
        if (ClearPath(x, y, CellX(g_route[i]), CellY(g_route[i])))
            best = i;
        if (IsTeleportHop(i))
            break;
    }
    *tx = CellX(g_route[best]);
    *ty = CellY(g_route[best]);

    // At a teleporter, walk on through its line.
    if (IsTeleportHop(best) || (best == g_progress && IsTeleportHop(g_progress)))
    {
        int c = g_route[best];
        for (int t = 0; t < g_numTele; t++)
            if (g_tele[t].fromCell == c)
            {
                // Push across: aim a cell beyond, away from the front side.
                for (int i = 0; i < numlines; i++)
                {
                    line_t* l = &lines[i];
                    if ((l->special != 39 && l->special != 97))
                        continue;
                    float ax = l->v1->x / (float)FRACUNIT, ay = l->v1->y / (float)FRACUNIT;
                    float bx = l->v2->x / (float)FRACUNIT, by = l->v2->y / (float)FRACUNIT;
                    if (SegmentDistance(*tx, *ty, ax, ay, bx, by) > 13)
                        continue;
                    float nx = (by - ay), ny = -(bx - ax);   // points to the front
                    float len = sqrtf(nx * nx + ny * ny);
                    *tx = (ax + bx) / 2 - nx / len * 24;
                    *ty = (ay + by) / 2 - ny / len * 24;
                    return;
                }
            }
    }
}

// ---------------------------------------------------------------------------
// Doors, lifts and the exit

static void OpenDoor(sector_t* sec)
{
    if (sec->specialdata)
    {
        thinker_t* th = sec->specialdata;
        if (th->function.acp1 == (actionf_p1)T_VerticalDoor && ((vldoor_t*)th)->direction == -1)
        {
            ((vldoor_t*)th)->direction = 1;    // closing on us: open again
            S_StartSound((mobj_t*)&sec->soundorg, sfx_doropn);
        }
        return;
    }
    fixed_t top = g_secOpenCeiling[SectorIndex(sec)];
    if (top <= sec->ceilingheight)
        return;

    vldoor_t* door = Z_Malloc(sizeof(*door), PU_LEVSPEC, 0);
    P_AddThinker(&door->thinker);
    sec->specialdata = door;
    door->thinker.function.acp1 = (actionf_p1)T_VerticalDoor;
    door->sector = sec;
    door->type = normal;
    door->topheight = top;
    door->direction = 1;
    door->speed = VDOORSPEED;
    door->topwait = VDOORWAIT;
    S_StartSound((mobj_t*)&sec->soundorg, sfx_doropn);
}

static void LowerLift(sector_t* sec)
{
    if (sec->specialdata)
        return;   // already moving
    fixed_t low = g_secLowFloor[SectorIndex(sec)];
    if (low >= sec->floorheight)
        return;

    plat_t* plat = Z_Malloc(sizeof(*plat), PU_LEVSPEC, 0);
    P_AddThinker(&plat->thinker);
    plat->type = downWaitUpStay;
    plat->sector = sec;
    plat->sector->specialdata = plat;
    plat->thinker.function.acp1 = (actionf_p1)T_PlatRaise;
    plat->crush = false;
    plat->tag = sec->tag;
    plat->speed = PLATSPEED * 4;
    plat->low = low;
    plat->high = sec->floorheight;
    plat->wait = TICRATE * PLATWAIT;
    plat->status = down;
    S_StartSound((mobj_t*)&sec->soundorg, sfx_pstart);
    P_AddActivePlat(plat);
}

// Trigger the lines that move a sector's floor, each once, as the player would.
static void TriggerMovers(int s, mobj_t* mo)
{
    for (int k = 0; k < MAX_MOVERS && g_secMovers[s][k] >= 0; k++)
    {
        int i = g_secMovers[s][k];
        line_t* l = &lines[i];
        if (g_lineTriggered[i] || !l->special)
            continue;
        g_lineTriggered[i] = 1;
        LOGI("Ride: triggering line %d (special %d) to move sector %d", i, l->special, s);
        if (IsWalkSpecial(l->special))
            P_CrossSpecialLine(i, 0, mo);
        else if (IsGunSpecial(l->special))
            P_ShootSpecialLine(mo, l);
        else
            P_UseSpecialLine(mo, l, 0);
    }
}

static void OpenTheWay(mobj_t* mo)
{
    sector_t* here = mo->subsector->sector;
    int prev = SectorIndex(here);
    int last = -1;
    for (int i = g_progress; i < g_routeLen && i <= g_progress + DOOR_LOOKAHEAD; i++)
    {
        int s = g_cellSector[g_route[i]];
        if (s == last)
            continue;
        last = s;
        sector_t* sec = &sectors[s];
        fixed_t floor = sec->floorheight > mo->floorz ? sec->floorheight : mo->floorz;

        if (sec->ceilingheight - floor < PLAYER_HEIGHT * FRACUNIT)
            OpenDoor(sec);

        if (g_secFlags[s] & SF_LIFT)
        {
            if (sec->floorheight - mo->floorz > MAX_STEP * FRACUNIT)
                LowerLift(sec);        // waiting below a raised lift
        }
        if ((g_secFlags[SectorIndex(here)] & SF_LIFT) && sec != here &&
            sec->floorheight < here->floorheight - MAX_STEP * FRACUNIT &&
            !ClearPath(mo->x / (float)FRACUNIT, mo->y / (float)FRACUNIT, CellX(g_route[i]), CellY(g_route[i])))
            LowerLift(here);           // riding a raised lift down

        // A step too high: raise the floor we are on (stairs) or lower the one ahead.
        if (sec->floorheight - sectors[prev].floorheight > MAX_STEP * FRACUNIT)
        {
            if (g_secHighFloor[prev] > sectors[prev].floorheight)
                TriggerMovers(prev, mo);
            else if (!(g_secFlags[s] & SF_LIFT) && g_secLowFloor[s] < sec->floorheight)
                TriggerMovers(s, mo);
        }
        prev = s;
    }
}

static int CheckExit(mobj_t* mo)
{
    if (g_exited)
        return 1;   // a switch clears its special once used: don't trigger it twice
    float x = mo->x / (float)FRACUNIT, y = mo->y / (float)FRACUNIT;
    for (int i = 0; i < g_numExitLines; i++)
    {
        line_t* l = g_exitLines[i];
        if (SegmentDistance(x, y, l->v1->x / (float)FRACUNIT, l->v1->y / (float)FRACUNIT,
                            l->v2->x / (float)FRACUNIT, l->v2->y / (float)FRACUNIT) > EXIT_REACH)
            continue;
        LOGI("Ride: reached the exit (line special %d)", l->special);
        g_exited = 1;
        if (l->special == 11 || l->special == 51)
            P_UseSpecialLine(mo, l, 0);
        else
            P_CrossSpecialLine(l - lines, 0, mo);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Targets

static int ValidTarget(mobj_t* m)
{
    return m && m->health > 0 && (m->flags & MF_SHOOTABLE) && !m->player &&
           ((m->flags & MF_COUNTKILL) || m->type == MT_SKULL);
}

static mobj_t* NearestTarget(mobj_t* pmo)
{
    mobj_t* best = NULL;
    float bestD = TARGET_RANGE * TARGET_RANGE;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (!ValidTarget(m))
            continue;
        float d = Dist2(pmo->x / (float)FRACUNIT, pmo->y / (float)FRACUNIT,
                        m->x / (float)FRACUNIT, m->y / (float)FRACUNIT);
        if (d < bestD && P_CheckSight(pmo, m))
        {
            bestD = d;
            best = m;
        }
    }
    return best;
}

// The monster drawn under the touch, using the sprites of the last frame drawn.
static mobj_t* TouchedMonster(float u, float v)
{
    int sx = (int)(u * SCREENWIDTH), sy = (int)(v * SCREENHEIGHT);
    mobj_t* best = NULL;
    fixed_t bestScale = 0;
    for (vissprite_t* vis = vissprites; vis < vissprite_p; vis++)
    {
        if (!ValidTarget(vis->mobj))
            continue;
        int x1 = viewwindowx + (vis->x1 << detailshift) - TOUCH_MARGIN;
        int x2 = viewwindowx + ((vis->x2 + 1) << detailshift) + TOUCH_MARGIN;
        int y1 = viewwindowy + ((centeryfrac - FixedMul(vis->gzt - viewz, vis->scale)) >> FRACBITS) - TOUCH_MARGIN;
        int y2 = viewwindowy + ((centeryfrac - FixedMul(vis->gz - viewz, vis->scale)) >> FRACBITS) + TOUCH_MARGIN;
        if (sx < x1 || sx > x2 || sy < y1 || sy > y2)
            continue;
        if (vis->scale > bestScale)   // nearest wins
        {
            bestScale = vis->scale;
            best = vis->mobj;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Per tic

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int TurnTowards(angle_t current, angle_t wanted, int maxTurn)
{
    int delta = (int)(wanted - current) >> 16;   // signed, 65536 per turn
    return Clamp(delta / 2, -maxTurn, maxTurn);
}

// The way to move while fighting a boss: circle it between BOSS_KEEP_MIN and BOSS_KEEP_MAX,
// sidestep shots heading our way, and only ever take a heading that is clear of walls.
static angle_t BossSteer(mobj_t* mo, mobj_t* boss)
{
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    float ax = px - boss->x / (float)FRACUNIT, ay = py - boss->y / (float)FRACUNIT;
    float d = sqrtf(ax * ax + ay * ay);
    if (d < 1) d = 1;
    ax /= d;
    ay /= d;                                  // unit vector away from the boss

    // Round the boss, pulled back into the band if too close or too far.
    float wx = -ay * g_orbit, wy = ax * g_orbit;
    float radial = 0;
    if (d < BOSS_KEEP_MIN)
        radial = 1.5f * (BOSS_KEEP_MIN - d) / BOSS_KEEP_MIN + 0.5f;
    else if (d > BOSS_KEEP_MAX)
        radial = -fminf(1.5f, (d - BOSS_KEEP_MAX) / 200.0f + 0.3f);
    wx += ax * radial;
    wy += ay * radial;

    // Dodge: for each shot, where will it pass closest to us, and when?
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (!(m->flags & MF_MISSILE) || m->target == mo)
            continue;
        float vx = m->momx / (float)FRACUNIT, vy = m->momy / (float)FRACUNIT;
        float vv = vx * vx + vy * vy;
        if (vv < 1)
            continue;
        float rx = px - m->x / (float)FRACUNIT, ry = py - m->y / (float)FRACUNIT;
        float t = (rx * vx + ry * vy) / vv;   // tics until closest
        if (t < 0 || t > DODGE_TICS)
            continue;
        float cx = rx - vx * t, cy = ry - vy * t;   // from the shot's path to us
        float miss = sqrtf(cx * cx + cy * cy);
        if (miss > DODGE_RADIUS)
            continue;
        if (miss < 1)
        {
            // Dead on: step to the side we are already circling towards.
            cx = -vy * g_orbit;
            cy = vx * g_orbit;
            miss = sqrtf(cx * cx + cy * cy);
        }
        float urgency = 3.0f * (1 - miss / DODGE_RADIUS) * (1 - t / (DODGE_TICS + 1)) + 1.0f;
        wx += cx / miss * urgency;
        wy += cy / miss * urgency;
    }

    // Best clear heading of 16.
    float bestScore = -2;
    float bestX = wx, bestY = wy;
    for (int k = 0; k < 16; k++)
    {
        float hx = cosf(k * (float)M_PI / 8), hy = sinf(k * (float)M_PI / 8);
        if (!ClearPath(px, py, px + hx * 48, py + hy * 48))
            continue;
        float len = sqrtf(wx * wx + wy * wy);
        float score = len > 0 ? (hx * wx + hy * wy) / len : 0;
        if (score > bestScore) { bestScore = score; bestX = hx; bestY = hy; }
    }

    // Boxed in on this side: go round the other way.
    if (bestScore < 0.2f)
    {
        if (++g_orbitBlocked > TICRATE / 2)
        {
            g_orbit = -g_orbit;
            g_orbitBlocked = 0;
        }
    }
    else
        g_orbitBlocked = 0;

    return R_PointToAngle2(mo->x, mo->y, mo->x + (fixed_t)(bestX * 64 * FRACUNIT), mo->y + (fixed_t)(bestY * 64 * FRACUNIT));
}

int RIDE_ScaleDamage(int damage)
{
    float scaled = damage * g_firepower;
    return scaled < 1 ? 1 : (int)(scaled + 0.5f);
}

static int g_fighting;   // circling a boss this tic

int RIDE_IsActive(void)
{
    return g_enabled && (g_routeState == ROUTE_OK || g_fighting) && gamestate == GS_LEVEL && !demoplayback;
}

float RIDE_Firepower(void)
{
    return g_firepower;
}

int RIDE_HasTarget(void)
{
    return g_target != NULL;
}

void RIDE_BuildTiccmd(ticcmd_t* cmd)
{
    int enabled, live, touchDown;
    float pace, resistance, u, v;

    pthread_mutex_lock(&g_inputLock);
    enabled = g_enabled;
    live = g_bikeLive;
    pace = g_pace;
    resistance = g_resistance;
    touchDown = g_touchDown;
    u = g_touchU;
    v = g_touchV;
    pthread_mutex_unlock(&g_inputLock);

    // Firepower: 0.5x to 3x from pace, raised by resistance. Without a bike it stays 1x.
    if (live)
    {
        float speed = pace / FULL_PACE;
        if (speed > 1) speed = 1;
        g_firepower = 0.5f + 2.5f * speed * (0.4f + 0.6f * resistance);
    }
    else
        g_firepower = 1.0f;

    if (gamestate != GS_LEVEL || demoplayback || netgame)
        return;

    player_t* player = &players[consoleplayer];
    mobj_t* mo = player->mo;
    if (!mo)
        return;

    if (g_builtLevel != g_level)
    {
        FreeLevel();
        BuildGrid();
        g_builtLevel = g_level;
    }
    if (!enabled)
        return;

    if (player->playerstate == PST_DEAD)
    {
        if (touchDown)
            cmd->buttons |= BT_USE;   // touch to restart
        return;
    }

    // Boss level: fight while the bosses live; once they are dead and their walls have moved,
    // build the grid again and ride on to the end.
    float bossDist = 0;
    mobj_t* boss = NearestBoss(mo, &bossDist);
    if (!boss && g_numBossSpots && !g_bossCleared)
    {
        if (!BossFloorsSettled())
        {
            g_fighting = 0;
            return;   // stand still while the way opens
        }
        LOGI("Ride: bosses defeated, planning the way to the end");
        g_bossCleared = 1;
        FreeLevel();
        BuildGrid();
        g_bossCleared = 1;   // BuildGrid found no bosses; keep it that way
    }
    int wasFighting = g_fighting;
    if (boss && bossDist < BOSS_ENGAGE && P_CheckSight(mo, boss))
        g_bossUnseen = 0;
    else
        g_bossUnseen++;
    g_fighting = boss && (g_bossUnseen == 0 || (wasFighting && g_bossUnseen < BOSS_UNSEEN_TICS));
    if (g_fighting != wasFighting)
        LOGI("Ride: %s", g_fighting ? "fighting the boss" : "not fighting");

    float x = mo->x / (float)FRACUNIT, y = mo->y / (float)FRACUNIT;
    if (!g_fighting)
    {
        if (g_routeState == ROUTE_NONE)
        {
            if (boss)
                MarkBossSpots();   // head for where the bosses are now
            PlanRoute(mo, boss ? CF_BOSS : CF_GOAL);
        }
        if (g_routeState != ROUTE_OK)
        {
            // Try again now and then: a door or lift may have opened the way.
            if (gametic - g_lastPlanTic > REPLAN_TICS * 5)
                g_routeState = ROUTE_NONE;
            return;
        }

        UpdateProgress(x, y);
        if (g_routeState != ROUTE_OK)
            return;

        // At the end of the approach with the bosses still out of sight: they have moved.
        if (boss && g_progress >= g_routeLen - 2)
        {
            if (gametic - g_lastPlanTic > REPLAN_TICS)
                g_routeState = ROUTE_NONE;
        }
        else
        {
            if (CheckExit(mo))
                return;
            OpenTheWay(mo);
        }
    }
    else
        g_routeState = ROUTE_NONE;   // plan afresh after the fight

    // Targeting
    if (touchDown)
    {
        mobj_t* touched = TouchedMonster(u, v);
        if (touched)
        {
            g_target = touched;
            g_targetTapped = 1;
            g_targetUnseen = 0;
        }
    }
    if (!ValidTarget(g_target))
    {
        g_target = NULL;
        g_targetTapped = 0;
    }
    if (g_target && !P_CheckSight(mo, g_target))
    {
        if (++g_targetUnseen > (g_targetTapped ? TAPPED_LOST_TICS : TARGET_TICS))
            g_target = NULL;
    }
    else
        g_targetUnseen = 0;
    if (!g_targetTapped && gametic % TARGET_TICS == 0)
        g_target = NearestTarget(mo);

    // Steering
    angle_t moveAngle;
    if (g_fighting)
    {
        moveAngle = BossSteer(mo, boss);
        if (!g_targetTapped || !g_target)
            g_target = boss;   // keep the boss in front
    }
    else
    {
        float tx, ty;
        Lookahead(x, y, &tx, &ty);
        moveAngle = R_PointToAngle2(mo->x, mo->y, (fixed_t)(tx * FRACUNIT), (fixed_t)(ty * FRACUNIT));
    }
    // Blocked for a second (usually a monster in the way): sidestep, a different side each time.
    if (!g_fighting && g_stuckTics > TICRATE)
    {
        if (g_stuckTics % TICRATE == 0)
            g_nudgeSign = -g_nudgeSign;
        moveAngle += g_nudgeSign * (ANG45 + ANG45 / 3);
    }
    angle_t faceAngle = g_target ? R_PointToAngle2(mo->x, mo->y, g_target->x, g_target->y) : moveAngle;
    int turn = TurnTowards(mo->angle, faceAngle, g_target ? 1536 : 1024);

    float speedFrac = pace / FULL_PACE;
    if (speedFrac > 1) speedFrac = 1;
    if (pace < 0.2f) speedFrac = 0;
    int speed = (int)(speedFrac * MAX_MOVE);

    angle_t rel = moveAngle - (mo->angle + ((angle_t)turn << 16));
    int fine = rel >> ANGLETOFINESHIFT;
    int forward = FixedMul(speed << FRACBITS, finecosine[fine]) >> FRACBITS;
    int side = -(FixedMul(speed << FRACBITS, finesine[fine]) >> FRACBITS);

    if (RIDE_TRACE && gametic % TICRATE == 0 && g_routeState == ROUTE_OK)
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "Ride trace: at (%.0f,%.0f) progress %d/%d target %s speed %d",
                            x, y, g_progress, g_routeLen, g_target ? "yes" : "no", speed);

    cmd->forwardmove = (signed char)Clamp(forward, -MAX_MOVE, MAX_MOVE);
    cmd->sidemove = (signed char)Clamp(side, -MAX_MOVE, MAX_MOVE);
    cmd->angleturn = (short)turn;

    // Fire while the rider holds a monster, once roughly facing it.
    cmd->buttons &= ~BT_ATTACK;
    if (touchDown && g_target && g_targetTapped)
    {
        int off = (int)(faceAngle - mo->angle);
        if (off < 0) off = -off;
        if (off < (int)(ANG45 / 4))
            cmd->buttons |= BT_ATTACK;
    }
    if (!touchDown)
        g_targetTapped = 0;

    // Stuck against something while pedalling: plan again. Not while a lift or door here or
    // just ahead is still moving: that is waiting, not stuck.
    int waiting = mo->subsector->sector->specialdata != NULL;
    if (g_routeState == ROUTE_OK && g_progress + 4 < g_routeLen)
        waiting |= sectors[g_cellSector[g_route[g_progress + 4]]].specialdata != NULL;
    if (waiting)
        g_stuckTics = 0;
    if (speed > 0 && !g_fighting)
    {
        if (abs(mo->x - g_stuckX) + abs(mo->y - g_stuckY) > 8 * FRACUNIT)
        {
            g_stuckX = mo->x;
            g_stuckY = mo->y;
            g_stuckTics = 0;
        }
        else if (++g_stuckTics > STUCK_TICS * 2 && gametic - g_lastPlanTic > REPLAN_TICS)
        {
            int ahead = g_route[g_progress + 3 < g_routeLen ? g_progress + 3 : g_routeLen - 1];
            sector_t* hs = mo->subsector->sector;
            sector_t* as = &sectors[g_cellSector[ahead]];
            LOGI("Ride: stuck at (%d,%d) sector %d floor %d ceil %d; heading for (%.0f,%.0f) sector %d "
                 "floor %d ceil %d tag %d moving %d; planning again",
                 mo->x >> FRACBITS, mo->y >> FRACBITS, SectorIndex(hs), hs->floorheight >> FRACBITS,
                 hs->ceilingheight >> FRACBITS, CellX(ahead), CellY(ahead), g_cellSector[ahead],
                 as->floorheight >> FRACBITS, as->ceilingheight >> FRACBITS, as->tag, as->specialdata != NULL);
            g_stuckTics = 0;
            g_routeState = ROUTE_NONE;
        }
    }
}
