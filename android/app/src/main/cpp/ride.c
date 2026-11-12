// Ride mode: the bike moves the player along a route through the level.
//
// When a level starts, the map is rasterised into a grid of 8-unit cells marking where the
// player fits, and A* finds a route from the player to the exit. Every tic RIDE_BuildTiccmd
// replaces the player's movement:
//
// - Speed comes from the bike's pace (cadence), so stopping pedalling stops the player.
// - The player faces the nearest visible monster (or the one the rider touched) and strafes so
//   the route is still followed; with no monster in sight the player faces along the route.
// - Monster shots about to hit the player are sidestepped, and so are imps and other projectile
//   throwers winding up an attack at the player: they aim where the player is as the shot leaves.
//   This works even when the rider has stopped pedalling; a stopped rider brakes to a halt after.
// - The route is a tour: the nearest key not yet held, then the nearest switch or trigger line that
//   opens or moves something, and only then the exit (the secret exit on maps that lead to a
//   secret level, so every level is played). Doors open as the player reaches them only if they
//   are unlocked or the player has the key; doors and floors worked from elsewhere only once their
//   line has been triggered. Lifts lower as the player reaches them. After triggering a line, the
//   player holds still until what it moves stops, facing it (or a monster, which comes first) and
//   still firing. If the exit cannot be reached that way, doors and floors on the route are opened
//   and moved regardless.
// - Reaching the exit line or switch ends the level.
// - Holding a finger on a monster's sprite targets it and fires. Pedalling at or above the
//   auto-fire cadence (by skill level, or set) fires at the nearest monster in sight on its own.
// - Boss levels (E1M8 and the like, which end when the bosses die rather than at an exit): ride
//   to the bosses, then circle the nearest at a distance, sidestepping their shots, until they
//   are dead. Then ride on to the level's end: an exit, or the sector that ends it (E1M8's
//   teleporter into a deadly room).
// - Pedal power (pace, scaled up by resistance) picks the weapon, from the pistol up to the BFG.
//   The rocket launcher is only used when its splash cannot reach the player. Ammo is unlimited.
// - Barrels: no shot is fired that would set off a barrel (or a chain of them) whose blast reaches
//   the player. Targets with a clear shot are preferred; otherwise the rider backs away from the
//   barrel and steps aside to take it out of the line of fire, for up to 3 seconds at a time.
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
#define RIDE_COMBAT_LOG 0       // 1: log hits on the player and shots held back for barrels

#define CELL            8       // grid cell size, map units
#define CLEARANCE       16.5f   // half-size of the box kept clear of walls (the player's is 16)
#define NEAR_BAND       24.0f   // half-size of the box marking cells close to walls (CF_NEAR)
#define NEAR_COST       6       // extra route cost per cell close to a wall
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
#define ROCKET_MIN_RANGE 256.0f // rockets only at targets this far (splash reaches 128) ...
#define ROCKET_CLEAR    192.0f  // ... and with no monster this close to the player
#define ROCKET_RETURN   64.0f   // extra distance needed to switch back to rockets
#define TIER_HYSTERESIS 0.15f   // fraction of a tier power must pass before changing weapon
#define SWITCH_TICS     (TICRATE / 2)   // at least this long between weapon changes
#define BOSS_APPROACH   160.0f  // ride until this close to a boss (it is fought once in sight)
#define BOSS_ENGAGE     1100.0f // fight a boss in sight this close
#define BOSS_UNSEEN_TICS (3*TICRATE) // keep fighting this long after losing sight of it
#define BOSS_KEEP_MIN   320.0f  // circle a boss between these distances
#define BOSS_KEEP_MAX   640.0f
#define DODGE_TICS      40      // react to a shot arriving within this many tics
#define DODGE_RADIUS    80.0f   // ... that would pass this close
#define MPS_PER_RPM     0.042f  // the bike library's cadence-to-pace mapping (CadencePace)
#define DETOUR_MIN      160.0f  // when stuck, detour to a random point this far ...
#define DETOUR_MAX      640.0f  // ... up to this far (route distance), then plan again
#define DETOUR_TICS     (10*TICRATE)  // give a detour up after this long
#define MAX_BARRELS     256
#define BARREL_BLAST    128.0f  // a barrel's blast: 128 damage less the distance (P_RadiusAttack)
#define BARREL_MARGIN   24.0f   // ... and this much more room kept from it
#define AIM_SLACK       0.06f   // radians either side of the aim allowed for turning and drift
#define EVADE_MAX_TICS  (3*TICRATE)   // back away from a barrel at most this long ...
#define EVADE_REST_TICS (2*TICRATE)   // ... then follow the route this long before trying again
#define SHOT_TICS       35      // step out of the way of a shot arriving within this many tics ...
#define SHOT_MARGIN     20.0f   // ... that would pass within this much of the player's edge
#define DODGE_MOVE      40      // dodge at least this fast (of MAX_MOVE), pedalling or not

// ---------------------------------------------------------------------------
// Inputs from the UI thread

static pthread_mutex_t g_inputLock = PTHREAD_MUTEX_INITIALIZER;
static int g_enabled = 1;
static int g_bikeLive;
static float g_pace;          // metres per second
static float g_resistance;    // 0..1
static int g_touchDown;
static float g_touchU, g_touchV;   // 0..1 across the 320x200 frame
static float g_autofireRpm;        // auto-fire cadence set by the rider; 0 for the skill's default
static int g_autofiring;           // pedalling fast enough to auto-fire; read by the UI
static float g_power;              // 0..1 pedal power; written by the game thread, read by the UI
static int g_weapon = -1;          // weapon ride mode has chosen, or -1; read by the UI
static int g_tiers[8], g_numTiers;   // weapons from weakest to strongest in this game
static int g_tier = -1;
static int g_lastSwitchTic = -1000;

// Episode totals (game thread)
static ridestats_t g_stats;
static float g_statsResistanceSum;   // resistance times seconds pedalling
static int g_statsLeveltime;

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

void RIDE_SetAutofireRpm(float rpm)
{
    pthread_mutex_lock(&g_inputLock);
    g_autofireRpm = rpm < 0 ? 0 : rpm;
    pthread_mutex_unlock(&g_inputLock);
}

// The cadence that switches auto-fire on: the rider's setting, else by skill (harder skills ask
// for faster pedalling).
float RIDE_AutofireRpm(void)
{
    static const float bySkill[] = { 45, 55, 65, 75, 85 };   // sk_baby .. sk_nightmare
    if (g_autofireRpm > 0)
        return g_autofireRpm;
    int skill = gameskill < 0 ? 2 : gameskill > 4 ? 4 : gameskill;
    return bySkill[skill];
}

int RIDE_Autofiring(void)
{
    return g_autofiring;
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
#define MAX_MOVERS 16
static int (*g_secMovers)[MAX_MOVERS]; // lines whose action moves the sector's floor, or -1
static int* g_secLock;               // key colour a manual door needs (0 blue, 1 yellow, 2 red), or -1
static fixed_t* g_secOpenCeiling;    // highest the ceiling can go (doors)

#define CF_WALL    1   // too close to a wall
#define CF_LEDGE   2   // too close to a ledge that is too high to climb (passable when dropping)
#define CF_GOAL    4
#define CF_HAZARD  8
#define CF_TELE    16  // a teleporter line starts here
#define CF_BOSS    32  // near where a boss started: the approach route's goal
#define CF_GOAL2   64  // near the other exit (normal if the secret one is wanted, or the reverse)
#define CF_NEAR    128 // close to a wall: routes avoid it, straight-line shortcuts may not cross it

#define SF_LIFT    1
#define SF_HAZARD  2
#define SF_MANUALDOOR 4   // a door opened by using its own line
#define SF_AVOID   8      // a lift that raises to its ceiling when its own edge is crossed or used
#define AVOID_COST 120    // extra route cost per cell in such a lift: only when there's no other way

typedef struct
{
    int fromCell;
    int toCell;
} teleedge_t;

static teleedge_t* g_tele;
static int g_numTele;

static line_t* g_exitLines[16];
static int g_exitFlag[16];   // CF_GOAL for the wanted exit, CF_GOAL2 for the other
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

// The tour (per level, kept across grid rebuilds)
enum { OBJ_NONE, OBJ_KEY, OBJ_LINE, OBJ_EXIT, OBJ_BOSS, OBJ_DETOUR };
#define LINE_DONE      1
#define LINE_ABANDONED 2
static byte* g_lineState;    // per line: LINE_DONE once triggered, LINE_ABANDONED if unreachable
static int g_linesDone;      // count of LINE_DONE, to know when the grid is out of date
static mobj_t* g_abandonedKeys[16];
static int g_numAbandonedKeys;
static int g_magic;          // fallback: open and move everything on the route regardless
static int g_preferSecret;   // this map's secret exit leads to a level not otherwise played
static int g_gridKeys = -1, g_gridLines = -1, g_gridMagic = -1;   // state the grid was built for
static int g_objKind;        // what the current route is heading for
static int g_objLine = -1;   // OBJ_LINE: the line
static mobj_t* g_objKey;     // OBJ_KEY: the key
static int g_objColor;       // OBJ_KEY: its colour
static int g_objExitFlag;    // OBJ_EXIT: CF_GOAL or CF_GOAL2
static int g_objStucks;      // times stuck while heading for this objective
static int g_objArrivedTic = -1;   // reached the end of the route without completing it
static int g_detourUntil;    // OBJ_DETOUR: when to stop and plan again
static int g_savedKind, g_savedLine, g_savedColor;   // the objective a detour interrupted
static mobj_t* g_savedKey;
static unsigned g_rideSeed = 0x9e3779b9u;   // our own random numbers: DOOM's affect demo sync
static int g_exited;         // the exit has been triggered this level
static mobj_t* g_target;
static int g_targetTapped;
static int g_targetUnseen;
static int g_stuckTics;
static int g_nudgeSign = 1;  // which way to sidestep something blocking the route
static mobj_t* g_barrels[MAX_BARRELS];   // live barrels, found again every tic
static int g_numBarrels;
static int g_evadeTics;      // tics spent backing away from a barrel in the line of fire
static int g_evadeRestUntil; // no backing away before this tic
static int g_evadeSide = 1;  // which way to step to take a barrel out of the line of fire
static int g_dodging;        // stepping out of a shot's path last tic
static int g_bestProgress;   // furthest route cell reached, and when: no progress for long is stuck
static int g_bestProgressTic;
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
    free(g_secLock); g_secLock = NULL;
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
    g_numTiers = 0;      // the game (and its weapons) may have changed
    g_tier = -1;
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

// The sectors a staircase line raises, following EV_BuildStairs; with plan set, also the heights
// it builds them to.
static void PlanStairs(int lineIndex, int plan)
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
            if (plan && height > g_secHighFloor[s])
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

static int SectorIndex(sector_t* s) { return s - sectors; }

// Manual doors (opened by using their own line): the key they need, -1 for none, or -2 if the
// special is not a manual door.
static int ManualDoorLock(int special)
{
    switch (special)
    {
      case 1: case 31: case 117: case 118: return -1;
      case 26: case 32: return 0;   // blue
      case 27: case 34: return 1;   // yellow
      case 28: case 33: return 2;   // red
    }
    return -2;
}

// Lines that open (not close) doors in the sectors they tag.
static int IsDoorOpenSpecial(int special)
{
    switch (special)
    {
      case 2: case 4: case 29: case 46: case 61: case 63: case 86: case 90: case 103:
      case 105: case 106: case 108: case 109: case 111: case 112: case 114: case 115:
        return 1;
    }
    return 0;
}

// Lines worth riding to: they open doors or move floors somewhere.
static int IsObjectiveSpecial(int special)
{
    return IsDoorOpenSpecial(special) || IsFloorMoverSpecial(special) || StairStep(special);
}

static int HasKey(int color)
{
    player_t* p = &players[consoleplayer];
    return p->cards[color] || p->cards[color + 3];   // keycard or skull key
}

static int KeyMask(void)
{
    return HasKey(0) | (HasKey(1) << 1) | (HasKey(2) << 2);
}

static int AnyMoverDone(int s)
{
    for (int k = 0; k < MAX_MOVERS && g_secMovers[s][k] >= 0; k++)
        if (g_lineState[g_secMovers[s][k]] == LINE_DONE)
            return 1;
    return 0;
}

// May the ride open this sector's door: unlocked or keyed manual door, or a door whose line has
// been triggered? (In the fallback, everything.)
static int DoorUnlocked(int s)
{
    if (g_magic)
        return 1;
    if ((g_secFlags[s] & SF_MANUALDOOR) && (g_secLock[s] < 0 || HasKey(g_secLock[s])))
        return 1;
    return AnyMoverDone(s);
}

static void ComputeSectorLimits(void)
{
    g_secFlags = calloc(numsectors, sizeof(int));
    g_secLock = malloc(numsectors * sizeof(int));
    g_secLowFloor = malloc(numsectors * sizeof(fixed_t));
    g_secHighFloor = malloc(numsectors * sizeof(fixed_t));
    g_secOpenCeiling = malloc(numsectors * sizeof(fixed_t));
    g_secMovers = malloc(numsectors * sizeof(*g_secMovers));
    memset(g_secMovers, 0xff, numsectors * sizeof(*g_secMovers));
    for (int s = 0; s < numsectors; s++)
    {
        g_secLowFloor[s] = g_secHighFloor[s] = sectors[s].floorheight;
        g_secLock[s] = -1;
    }

    // Manual doors: the sector behind the door line, and the key it needs.
    for (int i = 0; i < numlines; i++)
    {
        int lock = ManualDoorLock(lines[i].special);
        if (lock == -2 || !lines[i].backsector)
            continue;
        int s = SectorIndex(lines[i].backsector);
        g_secFlags[s] |= SF_MANUALDOOR;
        if (lock >= 0)
            g_secLock[s] = lock;
    }

    // Lifts that trap: raised to their ceiling (or crushing) by a walk-over or switch line on their
    // own edge, so stepping on or off can send the floor up with the player on it or inside it.
    for (int i = 0; i < numlines; i++)
    {
        line_t* l = &lines[i];
        int sp = l->special;
        if (!l->tag || !(sp == 5 || sp == 55 || sp == 56 || sp == 65 || sp == 91 || sp == 94))
            continue;
        for (int s = 0; s < numsectors; s++)
            if (sectors[s].tag == l->tag && (l->frontsector == &sectors[s] || l->backsector == &sectors[s]))
                g_secFlags[s] |= SF_AVOID;
    }

    // Sectors tagged by a lift line can be lowered.
    for (int i = 0; i < numlines; i++)
        if (IsLiftSpecial(lines[i].special) && lines[i].tag)
            for (int s = 0; s < numsectors; s++)
                if (sectors[s].tag == lines[i].tag)
                    g_secFlags[s] |= SF_LIFT;

    // Lines that open or move tagged sectors, and the floor heights those can reach (stairs: the
    // heights they are built to). The heights only count once a line has been triggered.
    for (int i = 0; i < numlines; i++)
    {
        line_t* l = &lines[i];
        if (!l->tag)
            continue;
        // A one-shot line clears its special once used; its floors are then where they are.
        // Trapping lifts are only ever planned where they are.
        int plan = l->special && (g_magic || g_lineState[i] == LINE_DONE);
        if (StairStep(l->special))
            PlanStairs(i, plan);
        else if (IsFloorMoverSpecial(l->special) || IsDoorOpenSpecial(l->special))
            for (int s = 0; s < numsectors; s++)
            {
                if (sectors[s].tag != l->tag)
                    continue;
                AddMover(s, i);
                if (!plan || !IsFloorMoverSpecial(l->special) || (g_secFlags[s] & SF_AVOID))
                    continue;
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

        // A door the ride may open is planned open: just below its lowest neighbouring ceiling, as
        // DOOM's doors open. Never a boss level's walls (tag 666, 667): only the bosses' deaths
        // open those.
        g_secOpenCeiling[s] = sec->ceilingheight;
        if (sec->ceilingheight - sec->floorheight < PLAYER_HEIGHT * FRACUNIT &&
            sec->tag != 666 && sec->tag != 667 && DoorUnlocked(s))
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
            MarkNearLine(l, NEAR_BAND, CF_NEAR, -1);
            continue;
        }

        int f = SectorIndex(l->frontsector), b = SectorIndex(l->backsector);
        fixed_t loF = g_secLowFloor[f], loB = g_secLowFloor[b];
        fixed_t hiF = g_secHighFloor[f], hiB = g_secHighFloor[b];
        fixed_t ceil = g_secOpenCeiling[f] < g_secOpenCeiling[b] ? g_secOpenCeiling[f] : g_secOpenCeiling[b];
        fixed_t floor = loF > loB ? loF : loB;

        if (ceil - floor < PLAYER_HEIGHT * FRACUNIT)
        {
            MarkNearLine(l, CLEARANCE, CF_WALL, -1);       // never enough headroom
            MarkNearLine(l, NEAR_BAND, CF_NEAR, -1);
        }
        else
        {
            // Too high to climb from the low side; the high side can still drop down.
            if (loB - hiF > MAX_STEP * FRACUNIT)
            {
                MarkNearLine(l, CLEARANCE, CF_LEDGE, 1);
                MarkNearLine(l, NEAR_BAND, CF_NEAR, 1);
            }
            if (loF - hiB > MAX_STEP * FRACUNIT)
            {
                MarkNearLine(l, CLEARANCE, CF_LEDGE, 0);
                MarkNearLine(l, NEAR_BAND, CF_NEAR, 0);
            }
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
        float near = mo->radius / (float)FRACUNIT + NEAR_BAND;
        float mx = mo->x / (float)FRACUNIT, my = mo->y / (float)FRACUNIT;
        int c0 = CellAt(mx - near, my - near), c1 = CellAt(mx + near, my + near);
        if (c0 < 0 || c1 < 0)
            continue;
        for (int cy = c0 / g_gridW; cy <= c1 / g_gridW; cy++)
            for (int cx = c0 % g_gridW; cx <= c1 % g_gridW; cx++)
            {
                int c = CellIndex(cx, cy);
                float dx = fabsf(CellX(c) - mx), dy = fabsf(CellY(c) - my);
                if (dx < r && dy < r)
                    g_cellFlags[c] |= CF_WALL;
                if (dx < near && dy < near)
                    g_cellFlags[c] |= CF_NEAR;
            }
    }

    // Exits: the wanted one (secret where it leads to a level not otherwise played, else normal)
    // is CF_GOAL, the other CF_GOAL2.
    for (int secret = 0; secret <= 1; secret++)
        for (int i = 0; i < numlines && g_numExitLines < 16; i++)
            if (IsExitSpecial(lines[i].special, secret))
            {
                g_exitFlag[g_numExitLines] = secret == g_preferSecret ? CF_GOAL : CF_GOAL2;
                g_exitLines[g_numExitLines++] = &lines[i];
            }
    for (int i = 0; i < g_numExitLines; i++)
        MarkNearLine(g_exitLines[i], EXIT_REACH - 8, g_exitFlag[i], -1);
    if (!g_numExitLines)
    {
        // No exit line (E1M8): the level ends in a sector with special 11.
        for (int c = 0; c < cells; c++)
            if (sectors[g_cellSector[c]].special == 11)
                g_cellFlags[c] |= CF_GOAL;
    }
    for (int c = 0; c < cells; c++)
        if ((g_cellFlags[c] & (CF_GOAL | CF_GOAL2)) && Walkable(c))
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

    int avoided = 0;
    for (int s = 0; s < numsectors; s++)
        if (g_secFlags[s] & SF_AVOID)
            avoided++;
    LOGI("Ride: %dx%d grid, %d exit lines, %d goal cells, %d teleporter cells, %d bosses, %d lifts avoided",
         g_gridW, g_gridH, g_numExitLines, g_numGoalCells, g_numTele, g_numBossSpots, avoided);
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

// A* from start to the nearest cell with goalFlag; with goalFlag 0, Dijkstra over everything
// reachable. Fills cost, parent and closed (allocated by the caller); returns the goal or -1.
static int Search(int start, int goalFlag, int* cost, int* parent, byte* closed)
{
    static const int dirs[8][3] = {
        { 1, 0, 10 }, { -1, 0, 10 }, { 0, 1, 10 }, { 0, -1, 10 },
        { 1, 1, 14 }, { 1, -1, 14 }, { -1, 1, 14 }, { -1, -1, 14 } };
    int cells = g_gridW * g_gridH;
    for (int i = 0; i < cells; i++)
        cost[i] = MAXINT;
    memset(closed, 0, cells);
#define H(c) (goalFlag ? Heuristic(c) : 0)
    g_heapSize = 0;
    cost[start] = 0;
    parent[start] = -1;
    HeapPush(H(start), start);
    int goal = -1;

    while (g_heapSize)
    {
        int c = HeapPop().cell;
        if (closed[c])
            continue;
        closed[c] = 1;
        if (goalFlag && (g_cellFlags[c] & goalFlag))
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
            int g = cost[c] + dirs[d][2] * steps + ((g_cellFlags[n] & CF_HAZARD) ? HAZARD_COST : 0) +
                    ((g_cellFlags[n] & CF_NEAR) ? NEAR_COST : 0) +
                    ((g_secFlags[g_cellSector[n]] & SF_AVOID) ? AVOID_COST : 0);
            if (g < cost[n])
            {
                cost[n] = g;
                parent[n] = c;
                HeapPush(g + H(n), n);
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
                    HeapPush(g + H(n), n);
                }
            }
    }

#undef H
    return goal;
}

static void PlanRoute(mobj_t* mo, int goalFlag)
{

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
    byte* closed = malloc(cells);
    int goal = Search(start, goalFlag, cost, parent, closed);

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
// The tour: keys, then switches and trigger lines, then the exit

// Secret exits that lead to a level not otherwise played.
static int PreferSecretExit(void)
{
    if (gamemode == commercial)
        return gamemap == 15 || gamemap == 31;
    switch (gameepisode)
    {
      case 1: return gamemap == 3;
      case 2: return gamemap == 5;
      case 3: return gamemap == 6;
      case 4: return gamemap == 2;
    }
    return 0;
}

// The key colour (0 blue, 1 yellow, 2 red) a thing is, or -1.
static int KeyColor(mobj_t* m)
{
    switch (mobjinfo[m->type].doomednum)
    {
      case 5: case 40: return 0;
      case 6: case 39: return 1;
      case 13: case 38: return 2;
    }
    return -1;
}

static int KeyAbandoned(mobj_t* m)
{
    for (int i = 0; i < g_numAbandonedKeys; i++)
        if (g_abandonedKeys[i] == m)
            return 1;
    return 0;
}

// How close the player must come to a line to trigger it: a switch is used from in front of it,
// a walk-over line is crossed.
static float LineReach(line_t* l)
{
    return IsWalkSpecial(l->special) ? 16.0f : 48.0f;
}

// The cheapest reached cell within reach of a point or a line, or -1.
static int BestCellNear(float x0, float y0, float x1, float y1, float reach, line_t* frontOf,
                        const int* cost, const byte* closed)
{
    int c0 = CellAt(fminf(x0, x1) - reach, fminf(y0, y1) - reach);
    int c1 = CellAt(fmaxf(x0, x1) + reach, fmaxf(y0, y1) + reach);
    if (c0 < 0 || c1 < 0)
        return -1;
    int best = -1;
    for (int cy = c0 / g_gridW; cy <= c1 / g_gridW; cy++)
        for (int cx = c0 % g_gridW; cx <= c1 % g_gridW; cx++)
        {
            int c = CellIndex(cx, cy);
            if (!closed[c] || (best >= 0 && cost[c] >= cost[best]))
                continue;
            if (SegmentDistance(CellX(c), CellY(c), x0, y0, x1, y1) > reach)
                continue;
            if (frontOf && !OnFront(frontOf, CellX(c), CellY(c)))
                continue;
            best = c;
        }
    return best;
}

// Does the tag belong to a lift that traps (SF_AVOID)? Its lines are not worth riding to.
static int TagsAvoided(int tag)
{
    for (int s = 0; s < numsectors; s++)
        if (sectors[s].tag == tag && (g_secFlags[s] & SF_AVOID))
            return 1;
    return 0;
}

static void SetRoute(int goal, const int* parent)
{
    g_bestProgress = 0;
    g_bestProgressTic = gametic;
    int len = 0;
    for (int c = goal; c != -1; c = parent[c])
        len++;
    g_route = malloc(len * sizeof(int));
    g_routeLen = len;
    for (int c = goal, i = len - 1; c != -1; c = parent[c], i--)
        g_route[i] = c;
    g_progress = 0;
    g_routeState = ROUTE_OK;
}

// A checksum of every floor and ceiling height: the grid is out of date when it changes.
static unsigned SectorHeightsStamp(void)
{
    unsigned h = 2166136261u;
    for (int s = 0; s < numsectors; s++)
    {
        h = (h ^ (unsigned)sectors[s].floorheight) * 16777619u;
        h = (h ^ (unsigned)sectors[s].ceilingheight) * 16777619u;
    }
    return h;
}

static unsigned g_gridHeights;

// Rebuild the grid if keys, triggered lines, the fallback, or any floor or ceiling has changed
// since it was built (stairs, for one, can take longer to build than we wait for them).
static void RebuildGridIfStale(void)
{
    int keys = KeyMask();
    unsigned heights = SectorHeightsStamp();
    if (keys == g_gridKeys && g_linesDone == g_gridLines && g_magic == g_gridMagic && heights == g_gridHeights)
        return;
    FreeLevel();
    BuildGrid();
    g_gridKeys = keys;
    g_gridLines = g_linesDone;
    g_gridMagic = g_magic;
    g_gridHeights = heights;
}

static int AnythingMoving(void);
static int g_planHoldUntil;
static int g_holdingSince = -1;    // first tic we held off falling back because things moved

static void PlanTour(mobj_t* mo)
{
    g_lastPlanTic = gametic;
    RebuildGridIfStale();
    free(g_route);
    g_route = NULL;
    g_routeLen = 0;
    g_routeState = ROUTE_FAILED;
    g_objKind = OBJ_NONE;
    g_objArrivedTic = -1;

    int start = NearestWalkable(mo->x / (float)FRACUNIT, mo->y / (float)FRACUNIT);
    if (start < 0)
    {
        LOGI("Ride: no route (player off the grid)");
        return;
    }

    int cells = g_gridW * g_gridH;
    int* cost = malloc(cells * sizeof(int));
    int* parent = malloc(cells * sizeof(int));
    byte* closed = malloc(cells);
    Search(start, 0, cost, parent, closed);

    int goal = -1;

    // 1. The nearest key not yet held.
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        int color = KeyColor(m);
        if (color < 0 || HasKey(color) || KeyAbandoned(m))
            continue;
        float kx = m->x / (float)FRACUNIT, ky = m->y / (float)FRACUNIT;
        int c = BestCellNear(kx, ky, kx, ky, 20, NULL, cost, closed);
        if (c >= 0 && (goal < 0 || cost[c] < cost[goal]))
        {
            goal = c;
            g_objKind = OBJ_KEY;
            g_objKey = m;
            g_objColor = color;
        }
    }

    // 2. The nearest switch or trigger line that opens or moves something.
    if (goal < 0)
        for (int i = 0; i < numlines; i++)
        {
            line_t* l = &lines[i];
            if (g_lineState[i] || !l->tag || !IsObjectiveSpecial(l->special) || IsGunSpecial(l->special) ||
                TagsAvoided(l->tag))
                continue;
            int c = BestCellNear(l->v1->x / (float)FRACUNIT, l->v1->y / (float)FRACUNIT,
                                 l->v2->x / (float)FRACUNIT, l->v2->y / (float)FRACUNIT,
                                 LineReach(l) - 4, IsWalkSpecial(l->special) ? NULL : l, cost, closed);
            if (c >= 0 && (goal < 0 || cost[c] < cost[goal]))
            {
                goal = c;
                g_objKind = OBJ_LINE;
                g_objLine = i;
            }
        }

    // 3. The wanted exit, else the other one; never while a boss level's bosses live.
    int bossesLive = NearestBoss(mo, NULL) != NULL;
    for (int pass = 0; pass < 2 && goal < 0 && !bossesLive; pass++)
    {
        int flag = pass ? CF_GOAL2 : CF_GOAL;
        for (int c = 0; c < cells; c++)
            if (closed[c] && (g_cellFlags[c] & flag) && (goal < 0 || cost[c] < cost[goal]))
                goal = c;
        if (goal >= 0)
        {
            g_objKind = OBJ_EXIT;
            g_objExitFlag = flag;
        }
    }

    if (goal >= 0)
        g_holdingSince = -1;
    if (goal >= 0)
    {
        SetRoute(goal, parent);
        switch (g_objKind)
        {
          case OBJ_KEY:
            LOGI("Ride: heading for the %s key, %d units", g_objColor == 0 ? "blue" : g_objColor == 1 ? "yellow" : "red",
                 cost[goal] * CELL / 10);
            break;
          case OBJ_LINE:
            LOGI("Ride: heading for line %d (special %d, tag %d), %d units", g_objLine,
                 lines[g_objLine].special, lines[g_objLine].tag, cost[goal] * CELL / 10);
            break;
          default:
            LOGI("Ride: heading for the %s exit, %d units",
                 (g_objExitFlag == CF_GOAL) == g_preferSecret ? "secret" : "normal", cost[goal] * CELL / 10);
        }
    }
    else if (!g_magic && AnythingMoving() && (g_holdingSince < 0 || gametic - g_holdingSince < 5 * TICRATE))
    {
        // Doors or floors still moving: what can be reached may change. Look again shortly, for
        // up to 5 seconds (monsters opening doors can keep something moving for ever).
        if (g_holdingSince < 0)
        {
            g_holdingSince = gametic;
            LOGI("Ride: nothing reachable yet; waiting for doors and floors to stop");
        }
        g_routeState = ROUTE_NONE;
        g_planHoldUntil = gametic + TICRATE;
    }
    else if (!g_magic)
    {
        // Nothing left to do and no way out: open and move whatever is in the way after all.
        LOGI("Ride: no way to the exit with what is unlocked; opening the way regardless");
        g_magic = 1;
        g_routeState = ROUTE_NONE;
    }
    else
        LOGI("Ride: no route to the exit");

    free(cost);
    free(parent);
    free(closed);
}

static void AbandonObjective(const char* why)
{
    LOGI("Ride: giving up on this objective (%s)", why);
    if (g_objKind == OBJ_KEY && g_numAbandonedKeys < 16)
        g_abandonedKeys[g_numAbandonedKeys++] = g_objKey;
    else if (g_objKind == OBJ_LINE)
        g_lineState[g_objLine] = LINE_ABANDONED;
    g_objKind = OBJ_NONE;
    g_objStucks = 0;
    g_routeState = ROUTE_NONE;
}

static int RideRandom(void)
{
    g_rideSeed = g_rideSeed * 1103515245u + 12345u;
    return (g_rideSeed >> 16) & 0x7fff;
}

// Stuck: ride to a random reachable point a little way off, clear of walls and away from here,
// then plan again (CheckObjective ends the detour). False if there is nowhere to go.
static int StartDetour(mobj_t* mo)
{
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    int start = NearestWalkable(px, py);
    if (start < 0)
        return 0;
    int cells = g_gridW * g_gridH;
    int* cost = malloc(cells * sizeof(int));
    int* parent = malloc(cells * sizeof(int));
    byte* closed = malloc(cells);
    Search(start, 0, cost, parent, closed);

    // A random pick among the candidates (reservoir sampling).
    int pick = -1, seen = 0;
    int lo = (int)(DETOUR_MIN / CELL * 10), hi = (int)(DETOUR_MAX / CELL * 10);
    for (int c = 0; c < cells; c++)
    {
        if (!closed[c] || cost[c] < lo || cost[c] > hi || (g_cellFlags[c] & (CF_NEAR | CF_HAZARD)) ||
            (g_secFlags[g_cellSector[c]] & SF_AVOID))
            continue;
        if (Dist2Cell(c, px, py) < DETOUR_MIN * DETOUR_MIN)
            continue;
        if (RideRandom() % ++seen == 0)
            pick = c;
    }

    if (pick >= 0)
    {
        if (g_objKind != OBJ_DETOUR)
        {
            g_savedKind = g_objKind;
            g_savedLine = g_objLine;
            g_savedKey = g_objKey;
            g_savedColor = g_objColor;
        }
        free(g_route);
        g_route = NULL;
        SetRoute(pick, parent);
        g_objKind = OBJ_DETOUR;
        g_objArrivedTic = -1;
        g_detourUntil = gametic + DETOUR_TICS;
        g_lastPlanTic = gametic;
        LOGI("Ride: detour to (%.0f,%.0f), %d units, then plan again", CellX(pick), CellY(pick), cost[pick] * CELL / 10);
    }
    free(cost);
    free(parent);
    free(closed);
    return pick >= 0;
}

static int g_settleLine = -1;      // a line just triggered: wait while its sectors move
static int g_settleUntil;
static int g_objSpecial;           // the objective line's special as it was triggered

// Is any door, floor or lift moving (perpetual lifts and crushers aside)? Planning mid-move sees
// half-open doors and half-raised floors.
// A door, floor or lift on the move (not a perpetual lift, which never stops).
static int SectorMoving(sector_t* sec)
{
    thinker_t* th = sec->specialdata;
    if (!th)
        return 0;
    if (th->function.acp1 == (actionf_p1)T_VerticalDoor || th->function.acp1 == (actionf_p1)T_MoveFloor)
        return 1;
    return th->function.acp1 == (actionf_p1)T_PlatRaise && ((plat_t*)th)->type != perpetualRaise;
}

static int AnythingMoving(void)
{
    for (int s = 0; s < numsectors; s++)
        if (SectorMoving(&sectors[s]))
            return 1;
    return 0;
}

// Did we just reach the objective? Keys are picked up by touching them; lines are triggered here.
static void CheckObjective(mobj_t* mo)
{
    if (g_objKind == OBJ_DETOUR)
    {
        if (g_progress >= g_routeLen - 2 || gametic > g_detourUntil)
        {
            LOGI("Ride: detour over, planning again");
            g_objKind = OBJ_NONE;
            g_routeState = ROUTE_NONE;
        }
        return;
    }
    if (g_objKind == OBJ_KEY && HasKey(g_objColor))
    {
        LOGI("Ride: got the key");
        g_objKind = OBJ_NONE;
        g_objStucks = 0;
        g_routeState = ROUTE_NONE;
        return;
    }
    if (g_objKind == OBJ_LINE)
    {
        line_t* l = &lines[g_objLine];
        float d = SegmentDistance(mo->x / (float)FRACUNIT, mo->y / (float)FRACUNIT,
                                  l->v1->x / (float)FRACUNIT, l->v1->y / (float)FRACUNIT,
                                  l->v2->x / (float)FRACUNIT, l->v2->y / (float)FRACUNIT);
        if (d <= LineReach(l))
        {
            g_objSpecial = l->special;
            LOGI("Ride: triggering line %d (special %d)", g_objLine, l->special);
            if (l->special)   // a walk-over line may already have gone off as we crossed it
            {
                if (IsWalkSpecial(l->special))
                    P_CrossSpecialLine(g_objLine, 0, mo);
                else
                    P_UseSpecialLine(mo, l, 0);
            }
            // Lines with the same action on the same tag (a door with a switch on each side) are
            // done too.
            for (int i = 0; i < numlines; i++)
                if (i == g_objLine ||
                    (lines[i].tag == l->tag && lines[i].special == g_objSpecial && g_lineState[i] != LINE_DONE))
                {
                    g_lineState[i] = LINE_DONE;
                    g_linesDone++;
                }
            g_settleLine = g_objLine;
            g_settleUntil = gametic + 10 * TICRATE;
            g_objKind = OBJ_NONE;
            g_objStucks = 0;
            g_routeState = ROUTE_NONE;
            return;
        }
    }

    // At the end of the route without getting it (a key on a ledge just out of reach, say).
    if (g_objKind != OBJ_NONE && g_objKind != OBJ_EXIT && g_progress >= g_routeLen - 2)
    {
        if (g_objArrivedTic < 0)
            g_objArrivedTic = gametic;
        else if (gametic - g_objArrivedTic > 3 * TICRATE)
            AbandonObjective("arrived but could not reach it");
    }
}

// After triggering a line, hold still (no new route) while the sectors it tagged move.
static int Settling(void)
{
    if (g_settleLine < 0)
        return 0;
    if (gametic < g_settleUntil)
    {
        int tag = lines[g_settleLine].tag;
        for (int s = 0; s < numsectors; s++)
            if (sectors[s].tag == tag && sectors[s].specialdata)
                return 1;
    }
    g_settleLine = -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Following the route

static float Dist2(float ax, float ay, float bx, float by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

// While holding still for sectors to move, the one to watch: the nearest moving sector the line
// just triggered tags (floor or ceiling), else the nearest door, floor or lift on the move. NULL if
// none, or if it is the one the player stands in.
static sector_t* WatchedSector(mobj_t* mo)
{
    int tag = g_settleLine >= 0 ? lines[g_settleLine].tag : -1;
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    sector_t* best = NULL;
    int bestTagged = 0;
    float bestD = 0;
    for (int s = 0; s < numsectors; s++)
    {
        sector_t* sec = &sectors[s];
        int tagged = sec->tag == tag && sec->specialdata;
        if (!tagged && !SectorMoving(sec))
            continue;
        float d = Dist2(px, py, sec->soundorg.x / (float)FRACUNIT, sec->soundorg.y / (float)FRACUNIT);
        if (!best || tagged > bestTagged || (tagged == bestTagged && d < bestD))
        {
            best = sec;
            bestTagged = tagged;
            bestD = d;
        }
    }
    return best == mo->subsector->sector ? NULL : best;
}

static int IsTeleportHop(int i)
{
    // Consecutive route cells more than a drop apart were joined by a teleporter.
    return i + 1 < g_routeLen && Dist2(CellX(g_route[i]), CellY(g_route[i]),
                                       CellX(g_route[i + 1]), CellY(g_route[i + 1])) > (float)(CELL * 5) * (CELL * 5);
}

// Straight walk across walkable cells from (x0,y0) to (x1,y1)?
// Can the player steer straight from (x0,y0) to (x1,y1)? Every cell on the way must be walkable,
// and away from walls: a shortcut that brushes a wall catches the player's corner on it. Cells
// near either end may be close to a wall (the player and the route are often there).
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
        if ((g_cellFlags[c] & CF_NEAR) && d * t > CELL * 1.5f && d * (1 - t) > CELL * 1.5f)
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

// Can the player not step from sector a to sector b as things are now?
static int BlockedStep(int a, int b)
{
    sector_t* sa = &sectors[a];
    sector_t* sb = &sectors[b];
    fixed_t ceil = sa->ceilingheight < sb->ceilingheight ? sa->ceilingheight : sb->ceilingheight;
    fixed_t floor = sa->floorheight > sb->floorheight ? sa->floorheight : sb->floorheight;
    // A closed door ahead is opened elsewhere; only floors count here.
    if (sb->ceilingheight - sb->floorheight < PLAYER_HEIGHT * FRACUNIT)
        return 0;
    return sb->floorheight - sa->floorheight > MAX_STEP * FRACUNIT || ceil - floor < PLAYER_HEIGHT * FRACUNIT;
}

static void SetRoute(int goal, const int* parent);
static int BestCellNear(float x0, float y0, float x1, float y1, float reach, line_t* frontOf,
                        const int* cost, const byte* closed);
static float LineReach(line_t* l);

// Route to within reach of a line (it is triggered on arrival, see CheckObjective).
static void RouteToLine(mobj_t* mo, int line)
{
    line_t* l = &lines[line];
    int cells = g_gridW * g_gridH;
    int start = NearestWalkable(mo->x / (float)FRACUNIT, mo->y / (float)FRACUNIT);
    free(g_route);
    g_route = NULL;
    g_routeLen = 0;
    g_routeState = ROUTE_FAILED;
    g_objArrivedTic = -1;
    g_lastPlanTic = gametic;
    if (start < 0)
        return;
    int* cost = malloc(cells * sizeof(int));
    int* parent = malloc(cells * sizeof(int));
    byte* closed = malloc(cells);
    Search(start, 0, cost, parent, closed);
    int goal = BestCellNear(l->v1->x / (float)FRACUNIT, l->v1->y / (float)FRACUNIT,
                            l->v2->x / (float)FRACUNIT, l->v2->y / (float)FRACUNIT,
                            LineReach(l) - 4, IsWalkSpecial(l->special) ? NULL : l, cost, closed);
    if (goal >= 0)
        SetRoute(goal, parent);
    else
    {
        LOGI("Ride: can't reach line %d", line);
        g_objKind = OBJ_NONE;
        g_routeState = ROUTE_NONE;
    }
    free(cost);
    free(parent);
    free(closed);
}

// Does a floor special lower (-1) or raise (+1) the floor?
static int FloorDirection(int special)
{
    switch (special)
    {
      case 19: case 23: case 36: case 37: case 38: case 45: case 60: case 70: case 71: case 82:
      case 83: case 84: case 98: case 102:
        return -1;
    }
    return IsFloorMoverSpecial(special) || StairStep(special) ? 1 : 0;
}

// A line that can move sector s's floor in direction dir again: repeatable (its special is still
// set), and worked by walking over or using it. -1 if none.
static int RepeatableMover(int s, int dir)
{
    for (int k = 0; k < MAX_MOVERS && g_secMovers[s][k] >= 0; k++)
    {
        line_t* l = &lines[g_secMovers[s][k]];
        if (l->special && !IsGunSpecial(l->special) && FloorDirection(l->special) == dir)
            return g_secMovers[s][k];
    }
    return -1;
}

// Trigger the lines that move a sector's floor, each once, as the player would.
static void TriggerMovers(int s, mobj_t* mo)
{
    for (int k = 0; k < MAX_MOVERS && g_secMovers[s][k] >= 0; k++)
    {
        int i = g_secMovers[s][k];
        line_t* l = &lines[i];
        if (g_lineState[i] == LINE_DONE || !l->special)
            continue;
        g_lineState[i] = LINE_DONE;
        g_linesDone++;
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

        // Doors open only if unlocked, keyed or triggered (OpenDoor stops at the planned height).
        if (sec->ceilingheight - floor < PLAYER_HEIGHT * FRACUNIT && DoorUnlocked(s))
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

        // A step the player can't make (too high, or no headroom between the floors and ceilings):
        // work a repeatable line that moves the floor we are on, or the one ahead, the right way,
        // the way a player rides a platform with its switch.
        if (!g_magic && s != prev && BlockedStep(prev, s))
        {
            int line = -1;
            if (sectors[prev].floorheight > sec->floorheight)
                line = RepeatableMover(prev, -1);           // lower the floor we are on
            else
            {
                line = RepeatableMover(s, -1);              // lower the one ahead
                if (line < 0)
                    line = RepeatableMover(prev, 1);        // or raise ours to meet it
            }
            if (line >= 0 && !(g_objKind == OBJ_LINE && g_objLine == line))
            {
                LOGI("Ride: the way on needs line %d (special %d) again", line, lines[line].special);
                g_objKind = OBJ_LINE;
                g_objLine = line;
                RouteToLine(mo, line);
                return;
            }
        }

        // Fallback only: a step too high, so raise the floor we are on (stairs) or lower the one
        // ahead by triggering the line that moves it.
        if (g_magic && sec->floorheight - sectors[prev].floorheight > MAX_STEP * FRACUNIT)
        {
            if (g_secHighFloor[prev] > sectors[prev].floorheight)
                TriggerMovers(prev, mo);
            else if (!(g_secFlags[s] & SF_LIFT) && g_secLowFloor[s] < sec->floorheight)
                TriggerMovers(s, mo);
        }
        prev = s;
    }
}

// At the exit the route is heading for: trigger it.
static int CheckExit(mobj_t* mo)
{
    if (g_exited)
        return 1;   // a switch clears its special once used: don't trigger it twice
    if (g_objKind != OBJ_EXIT)
        return 0;
    float x = mo->x / (float)FRACUNIT, y = mo->y / (float)FRACUNIT;
    for (int i = 0; i < g_numExitLines; i++)
    {
        line_t* l = g_exitLines[i];
        if (g_exitFlag[i] != g_objExitFlag)
            continue;
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

// ---------------------------------------------------------------------------
// Barrels: a shot that sets one off, or a chain of them, next to the player hurts the player.

static void FindBarrels(void)
{
    g_numBarrels = 0;
    for (thinker_t* th = thinkercap.next; th != &thinkercap && g_numBarrels < MAX_BARRELS; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (m->type == MT_BARREL && m->health > 0)
            g_barrels[g_numBarrels++] = m;
    }
}

// Where a weapon's shots can go: how far either side of the aim (radians), how wide each shot is,
// and how far it reaches.
static void WeaponSpread(int weapon, float* half, float* width, float* range)
{
    *half = AIM_SLACK;
    *width = 0;
    *range = MISSILERANGE / (float)FRACUNIT;
    switch (weapon)
    {
      case wp_fist:
      case wp_chainsaw:
        *range = MELEERANGE / (float)FRACUNIT + 16;
        break;
      case wp_pistol:
      case wp_chaingun:
      case wp_shotgun:
        *half += 0.098f;   // (P_Random() - P_Random()) << 18
        break;
      case wp_supershotgun:
        *half += 0.196f;   // ... << 19
        break;
      case wp_missile:
        *width = 11;       // the rocket's radius
        break;
      case wp_plasma:
        *width = 13;
        break;
      case wp_bfg:
        *half = 0.79f;     // its spray: 40 tracers across 90 degrees
        *width = 13;
        break;
    }
}

// The blast reach P_RadiusAttack measures: the larger of the x and y distances, less the
// victim's radius.
static float BlastDistance(mobj_t* spot, mobj_t* victim)
{
    float dx = fabsf((victim->x - spot->x) / (float)FRACUNIT);
    float dy = fabsf((victim->y - spot->y) / (float)FRACUNIT);
    return (dx > dy ? dx : dy) - victim->radius / (float)FRACUNIT;
}

// Would firing `weapon` from the player along the unit vector (dirX, dirY) set off a barrel whose
// blast, directly or through a chain of other barrels, reaches the player? If so, returns 1 with
// the nearest barrel in the line of fire in *lit and the one whose blast reaches the player in
// *hurts.
static int BarrelDanger(mobj_t* mo, float dirX, float dirY, int weapon, mobj_t** lit, mobj_t** hurts)
{
    if (!g_numBarrels)
        return 0;
    float half, width, range;
    WeaponSpread(weapon, &half, &width, &range);
    float tanHalf = tanf(half);
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;

    // Barrels in the line of fire.
    byte going[MAX_BARRELS];
    int queue[MAX_BARRELS], n = 0;
    mobj_t* firstLit = NULL;
    float firstAlong = 0;
    memset(going, 0, g_numBarrels);
    for (int i = 0; i < g_numBarrels; i++)
    {
        mobj_t* b = g_barrels[i];
        float bx = b->x / (float)FRACUNIT - px, by = b->y / (float)FRACUNIT - py;
        float along = bx * dirX + by * dirY;
        if (along <= 0 || along > range)
            continue;
        float lateral = fabsf(bx * dirY - by * dirX);
        if (lateral > along * tanHalf + width + b->radius / (float)FRACUNIT)
            continue;
        if (!P_CheckSight(mo, b))
            continue;
        going[i] = 1;
        queue[n++] = i;
        if (!firstLit || along < firstAlong)
        {
            firstLit = b;
            firstAlong = along;
        }
    }
    if (!n)
        return 0;

    // Barrels those set off in turn.
    for (int h = 0; h < n; h++)
    {
        mobj_t* a = g_barrels[queue[h]];
        for (int j = 0; j < g_numBarrels; j++)
            if (!going[j] && BlastDistance(a, g_barrels[j]) < BARREL_BLAST && P_CheckSight(a, g_barrels[j]))
            {
                going[j] = 1;
                queue[n++] = j;
            }
    }

    // The nearest of them whose blast reaches the player.
    mobj_t* worst = NULL;
    float worstD = BARREL_BLAST + BARREL_MARGIN;
    for (int h = 0; h < n; h++)
    {
        mobj_t* a = g_barrels[queue[h]];
        float d = BlastDistance(a, mo);
        if (d < worstD && P_CheckSight(a, mo))
        {
            worst = a;
            worstD = d;
        }
    }
    if (!worst)
        return 0;
    *lit = firstLit;
    *hurts = worst;
    return 1;
}

static int BarrelDangerTowards(mobj_t* mo, mobj_t* target, int weapon, mobj_t** lit, mobj_t** hurts)
{
    float dx = (target->x - mo->x) / (float)FRACUNIT, dy = (target->y - mo->y) / (float)FRACUNIT;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 1)
        return 0;
    return BarrelDanger(mo, dx / d, dy / d, weapon, lit, hurts);
}

// The nearest monster in sight, preferring one that can be shot without setting off a barrel
// next to the player.
static mobj_t* NearestTarget(mobj_t* pmo, int weapon)
{
    mobj_t* best = NULL;
    mobj_t* risky = NULL;
    float bestD = TARGET_RANGE * TARGET_RANGE, riskyD = bestD;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (!ValidTarget(m))
            continue;
        float d = Dist2(pmo->x / (float)FRACUNIT, pmo->y / (float)FRACUNIT,
                        m->x / (float)FRACUNIT, m->y / (float)FRACUNIT);
        if (d >= bestD || !P_CheckSight(pmo, m))
            continue;
        mobj_t *lit, *hurts;
        if (!BarrelDangerTowards(pmo, m, weapon, &lit, &hurts))
        {
            bestD = d;
            best = m;
        }
        else if (d < riskyD)
        {
            riskyD = d;
            risky = m;
        }
    }
    return best ? best : risky;
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

// Of 16 headings clear of walls for 48 units, the one nearest the wanted direction (wx, wy).
// *score: the cosine between them, or -2 if every way is blocked.
static angle_t ClearHeading(mobj_t* mo, float wx, float wy, float* score)
{
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    float len = sqrtf(wx * wx + wy * wy);
    int here = CellAt(px, py);
    int onHazard = here >= 0 && (g_cellFlags[here] & CF_HAZARD);
    float bestScore = -2;
    float bestX = wx, bestY = wy;
    for (int k = 0; k < 16; k++)
    {
        float hx = cosf(k * (float)M_PI / 8), hy = sinf(k * (float)M_PI / 8);
        if (!ClearPath(px, py, px + hx * 48, py + hy * 48))
            continue;
        // Nor onto damaging floor, as far as the player may slide.
        int hazard = 0;
        for (int r = CELL; r <= 96 && !onHazard && !hazard; r += CELL)
        {
            int c = CellAt(px + hx * r, py + hy * r);
            hazard = c >= 0 && (g_cellFlags[c] & CF_HAZARD);
        }
        if (hazard)
            continue;
        float s = len > 0 ? (hx * wx + hy * wy) / len : 0;
        if (s > bestScore) { bestScore = s; bestX = hx; bestY = hy; }
    }
    *score = bestScore;
    return R_PointToAngle2(mo->x, mo->y, mo->x + (fixed_t)(bestX * 64 * FRACUNIT), mo->y + (fixed_t)(bestY * 64 * FRACUNIT));
}

// Is this monster winding up a projectile attack (imp, cacodemon, baron and the like)? They aim at
// where the player is when the shot leaves, so being on the move by then makes it miss.
static int WindingUp(mobj_t* m)
{
    switch (m->type)
    {
      case MT_TROOP: case MT_HEAD: case MT_BRUISER: case MT_KNIGHT: case MT_UNDEAD:
      case MT_FATSO: case MT_BABY: case MT_CYBORG:
        break;
      default:
        return 0;
    }
    int st = mobjinfo[m->type].missilestate;
    for (int i = 0; i < 12 && st != S_NULL && st != mobjinfo[m->type].seestate; i++)
    {
        if (m->state == &states[st])
            return 1;
        st = states[st].nextstate;
    }
    return 0;
}

// A side step across the line (dx, dy), preferring the way we are going and then the side that is
// open.
static void SideStep(float px, float py, float dx, float dy, float moveX, float moveY, float* sx, float* sy)
{
    float len = sqrtf(dx * dx + dy * dy);
    *sx = -dy / len;
    *sy = dx / len;
    if (*sx * moveX + *sy * moveY < 0)
        *sx = -*sx, *sy = -*sy;
    if (!ClearPath(px, py, px + *sx * 48, py + *sy * 48))
        *sx = -*sx, *sy = -*sy;
}

// Monster shots about to hit the player: which way to step out of their paths, as a vector that is
// longer the more urgent, in *outX, *outY. Monsters winding up a shot at the player count too.
// Returns how many threats there are. (moveX, moveY): the way the player means to go, to pick a side
// when a shot is coming straight on.
static int ShotDodge(mobj_t* mo, float moveX, float moveY, float* outX, float* outY)
{
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    float pvx = mo->momx / (float)FRACUNIT, pvy = mo->momy / (float)FRACUNIT;
    float wx = 0, wy = 0;
    int n = 0;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (m->health > 0 && m->target == mo && WindingUp(m) && P_CheckSight(m, mo))
        {
            // About to throw: get moving across its aim, and back off one that is close enough
            // to claw instead.
            float dx = px - m->x / (float)FRACUNIT, dy = py - m->y / (float)FRACUNIT;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < 1)
                continue;
            float sx, sy;
            SideStep(px, py, dx, dy, moveX, moveY, &sx, &sy);
            wx += sx * 1.5f;
            wy += sy * 1.5f;
            if (d < 128)
            {
                wx += dx / d;
                wy += dy / d;
            }
            n++;
            continue;
        }
        // Shots, and lost souls charging: anything flying that isn't ours.
        if (!((m->flags & MF_MISSILE) && m->target != mo) && !((m->flags & MF_SKULLFLY) && m != mo))
            continue;
        // Its path relative to the player, who may already be stepping aside.
        float vx = m->momx / (float)FRACUNIT - pvx, vy = m->momy / (float)FRACUNIT - pvy;
        float vv = vx * vx + vy * vy;
        if (vv < 1)
            continue;
        float rx = px - m->x / (float)FRACUNIT, ry = py - m->y / (float)FRACUNIT;
        float t = (rx * vx + ry * vy) / vv;   // tics until closest
        if (t < 0 || t > SHOT_TICS)
            continue;
        float cx = rx - vx * t, cy = ry - vy * t;   // from the shot's path to the player
        float miss = sqrtf(cx * cx + cy * cy);
        float reach = (mo->radius + m->radius) / (float)FRACUNIT + SHOT_MARGIN;
        if (miss > reach)
            continue;
        // Passing over or under (fired from a ledge, say)?
        float mz = (m->z + m->momz * t) / (float)FRACUNIT;
        if (mz > (mo->z + mo->height) / (float)FRACUNIT + 8 || mz + m->height / (float)FRACUNIT < mo->z / (float)FRACUNIT - 8)
            continue;
        if (miss < 4)
        {
            // Straight on: step to the side that is open, preferring the way we are going.
            SideStep(px, py, vx, vy, moveX, moveY, &cx, &cy);
            miss = 1;
        }
        float urgency = 2.0f * (1 - miss / reach) + 2.0f * (1 - t / SHOT_TICS) + 1.0f;
        wx += cx / miss * urgency;
        wy += cy / miss * urgency;
        n++;
    }
    *outX = wx;
    *outY = wy;
    return n;
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

    float bestScore;
    angle_t heading = ClearHeading(mo, wx, wy, &bestScore);

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

    return heading;
}

// Back away from a barrel whose blast would reach the player, and step sideways to take the
// barrel in the line of fire out of it, towards the target. (dirX, dirY): unit vector to the
// target.
static angle_t BarrelSteer(mobj_t* mo, mobj_t* lit, mobj_t* hurts, float dirX, float dirY)
{
    float px = mo->x / (float)FRACUNIT, py = mo->y / (float)FRACUNIT;
    float ax = px - hurts->x / (float)FRACUNIT, ay = py - hurts->y / (float)FRACUNIT;
    float d = sqrtf(ax * ax + ay * ay);
    if (d < 1) { ax = -dirX; ay = -dirY; d = 1; }
    // The nearer the blast, the more backing away matters than a clear shot.
    float away = 0.5f + (BARREL_BLAST + BARREL_MARGIN - BlastDistance(hurts, mo)) / BARREL_BLAST;
    float wx = ax / d * away, wy = ay / d * away;

    // Step to the side of the line the barrel isn't on: that swings the line away from it.
    float lx = lit->x / (float)FRACUNIT - px, ly = lit->y / (float)FRACUNIT - py;
    float cross = dirX * ly - dirY * lx;   // > 0: barrel left of the line
    if (fabsf(cross) > 4)
        g_evadeSide = cross > 0 ? -1 : 1;
    wx += -dirY * g_evadeSide;   // (-dirY, dirX) is left; g_evadeSide -1 steps right
    wy += dirX * g_evadeSide;

    float score;
    angle_t heading = ClearHeading(mo, wx, wy, &score);
    if (score < 0.2f && fabsf(cross) <= 4)
        g_evadeSide = -g_evadeSide;   // walled in on this side: try the other
    return heading;
}

// ---------------------------------------------------------------------------
// Weapons: pedal power picks one


static void BuildTiers(void)
{
    g_numTiers = 0;
    g_tiers[g_numTiers++] = wp_pistol;
    g_tiers[g_numTiers++] = wp_shotgun;
    if (gamemode == commercial)
        g_tiers[g_numTiers++] = wp_supershotgun;
    g_tiers[g_numTiers++] = wp_chaingun;
    g_tiers[g_numTiers++] = wp_missile;
    if (gamemode != shareware)
    {
        g_tiers[g_numTiers++] = wp_plasma;
        g_tiers[g_numTiers++] = wp_bfg;
    }
}

// Is it safe to fire a rocket? Not at a target inside its blast, nor with a monster close by
// that could step into the shot. margin widens both distances.
static int RocketSafe(mobj_t* mo, float margin)
{
    if (g_target)
    {
        float dx = (g_target->x - mo->x) / (float)FRACUNIT, dy = (g_target->y - mo->y) / (float)FRACUNIT;
        if (sqrtf(dx * dx + dy * dy) - g_target->radius / (float)FRACUNIT < ROCKET_MIN_RANGE + margin)
            return 0;
    }
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mobj_t* m = (mobj_t*)th;
        if (m == mo || m->health <= 0 || !(m->flags & MF_SHOOTABLE))
            continue;
        float dx = (m->x - mo->x) / (float)FRACUNIT, dy = (m->y - mo->y) / (float)FRACUNIT;
        if (dx * dx + dy * dy < (ROCKET_CLEAR + margin) * (ROCKET_CLEAR + margin))
            return 0;
    }
    return 1;
}

static void ChooseWeapon(player_t* player)
{
    if (!g_numTiers)
        BuildTiers();

    // Power to a tier, holding the current one until power is clearly past its edges.
    float scaled = g_power * g_numTiers;
    int tier = (int)scaled;
    if (tier >= g_numTiers) tier = g_numTiers - 1;
    if (g_tier >= 0 && scaled > g_tier - TIER_HYSTERESIS && scaled < g_tier + 1 + TIER_HYSTERESIS)
        tier = g_tier;
    g_tier = tier;

    int weapon = g_tiers[tier];
    // Coming back to rockets needs a little more room than leaving them, so it doesn't flap.
    if (weapon == wp_missile &&
        !RocketSafe(player->mo, player->readyweapon == wp_missile ? 0 : ROCKET_RETURN))
        weapon = g_tiers[tier - 1];   // too close for rockets: the next one down
    g_weapon = weapon;

    player->weaponowned[weapon] = true;   // the bike chooses; no need to have found it
    if (player->readyweapon != weapon && player->pendingweapon != weapon &&
        gametic - g_lastSwitchTic > SWITCH_TICS)
    {
        player->pendingweapon = weapon;
        g_lastSwitchTic = gametic;
    }
}

static int g_fighting;   // circling a boss this tic

int RIDE_IsActive(void)
{
    // Active unless planning has failed (between routes, e.g. while a switch's doors move, too).
    return g_enabled && (g_routeState != ROUTE_FAILED || g_fighting) && gamestate == GS_LEVEL && !demoplayback;
}

float RIDE_Power(void)
{
    return g_power;
}

int RIDE_Weapon(void)
{
    return RIDE_IsActive() ? g_weapon : -1;
}

int RIDE_Objective(void)
{
    if (!RIDE_IsActive())
        return 0;
    int code = 0;
    switch (g_objKind)
    {
      case OBJ_KEY: code = 1 + g_objColor; break;                       // 1-3: blue, yellow, red key
      case OBJ_LINE: code = 4; break;                                    // a switch or trigger line
      case OBJ_EXIT: code = (g_objExitFlag == CF_GOAL) == g_preferSecret ? 6 : 5; break;   // 6: secret
      case OBJ_BOSS: code = 7; break;
      case OBJ_DETOUR: code = 8; break;
    }
    if (g_fighting)
        code = 7;
    return code | (g_magic ? 16 : 0);
}

void RIDE_NewGame(void)
{
    memset(&g_stats, 0, sizeof g_stats);
    g_statsResistanceSum = 0;
    g_statsLeveltime = 0;
}

void RIDE_LevelCompleted(void)
{
    if (demoplayback)
        return;
    player_t* p = &players[consoleplayer];
    g_stats.levels++;
    g_stats.seconds += leveltime / TICRATE;
    g_stats.kills += p->killcount;
    g_stats.maxKills += totalkills;
    g_stats.items += p->itemcount;
    g_stats.maxItems += totalitems;
    g_stats.secrets += p->secretcount;
    g_stats.maxSecrets += totalsecret;
}

void RIDE_EpisodeStats(ridestats_t* out)
{
    *out = g_stats;
    out->resistance = g_stats.ridingSeconds > 0 ? g_statsResistanceSum / g_stats.ridingSeconds : 0;
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

    // Pedal power, 0..1: pace, scaled up by resistance.
    {
        float speed = pace / FULL_PACE;
        if (speed > 1) speed = 1;
        g_power = speed * (0.4f + 0.6f * resistance);
    }

    // Episode totals: distance and pedalling, for each game tic played (none while paused).
    if (gamestate == GS_LEVEL && !demoplayback)
    {
        int dt = leveltime - g_statsLeveltime;
        g_statsLeveltime = leveltime;
        if (dt > 0 && dt <= TICRATE)
        {
            float secs = dt / (float)TICRATE;
            g_stats.meters += pace * secs;
            if (pace >= 0.2f)
            {
                g_stats.ridingSeconds += secs;
                g_statsResistanceSum += resistance * secs;
            }
        }
    }

    if (gamestate != GS_LEVEL || demoplayback || netgame)
        return;

    player_t* player = &players[consoleplayer];
    mobj_t* mo = player->mo;
    if (!mo)
        return;

    if (RIDE_COMBAT_LOG)
    {
        static int lastHealth = 100;
        if (player->health < lastHealth)
        {
            mobj_t* src = player->attacker;
            int boom = 0;   // barrels near the player in their death (explosion) frames
            for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next)
            {
                if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                    continue;
                mobj_t* m = (mobj_t*)th;
                if (m->type == MT_BARREL && m->health <= 0 && BlastDistance(m, mo) < BARREL_BLAST)
                    boom++;
            }
            LOGI("Ride damage: %d -> %d, attacker %s%d, exploding barrels near %d, sector special %d",
                 lastHealth, player->health, src == mo ? "self " : src ? "type " : "none ",
                 src && src != mo ? (int)src->type : 0, boom, mo->subsector->sector->special);
        }
        lastHealth = player->health;
    }

    // Unlimited ammo, in ride and free mode alike.
    for (int a = 0; a < NUMAMMO; a++)
        player->ammo[a] = player->maxammo[a];

    if (g_builtLevel != g_level)
    {
        free(g_lineState);
        g_lineState = calloc(numlines, 1);
        g_linesDone = 0;
        g_numAbandonedKeys = 0;
        g_magic = 0;
        g_preferSecret = PreferSecretExit();
        g_objKind = OBJ_NONE;
        g_objStucks = 0;
        g_settleLine = -1;
        g_evadeTics = 0;
        g_evadeRestUntil = 0;
        FreeLevel();
        BuildGrid();
        g_gridKeys = KeyMask();
        g_gridLines = 0;
        g_gridMagic = 0;
        g_gridHeights = SectorHeightsStamp();
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
        g_gridMagic = -1;    // the bosses' walls have moved: rebuild before planning
        g_routeState = ROUTE_NONE;
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
    // A line just triggered, or something is moving: hold still and let it finish first, watching
    // it, but keep fighting.
    int holding = !g_fighting && (Settling() || (g_routeState == ROUTE_NONE && gametic < g_planHoldUntil));
    if (g_fighting)
        g_routeState = ROUTE_NONE;   // plan afresh after the fight
    else if (!holding)
    {
        if (g_routeState == ROUTE_NONE)
        {
            if (boss)
            {
                RebuildGridIfStale();
                MarkBossSpots();   // head for where the bosses are now
                PlanRoute(mo, CF_BOSS);
                g_objKind = OBJ_BOSS;
                // No way to the bosses yet (E1M8 starts behind a switch): tour for keys and
                // switches, and try the bosses again after each one.
                if (g_routeState != ROUTE_OK)
                    PlanTour(mo);
            }
            else
                PlanTour(mo);
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
        if (boss && g_objKind == OBJ_BOSS && g_progress >= g_routeLen - 2)
        {
            if (gametic - g_lastPlanTic > REPLAN_TICS)
                g_routeState = ROUTE_NONE;
        }
        else
        {
            if (CheckExit(mo))
                return;
            CheckObjective(mo);
            if (g_routeState != ROUTE_OK)
                return;
            OpenTheWay(mo);
        }
    }

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
    FindBarrels();
    if (!g_targetTapped && gametic % TARGET_TICS == 0)
        g_target = NearestTarget(mo, player->readyweapon);

    ChooseWeapon(player);

    // Steering
    angle_t moveAngle;
    if (g_fighting)
    {
        moveAngle = BossSteer(mo, boss);
        if (!g_targetTapped || !g_target)
            g_target = boss;   // keep the boss in front
    }
    else if (holding)
    {
        // Face what is moving, unless a monster needs facing (faceAngle below).
        sector_t* watch = WatchedSector(mo);
        static sector_t* lastWatch;
        if (watch && watch != lastWatch)
            LOGI("Ride: waiting; watching sector %d (tag %d) move", SectorIndex(watch), watch->tag);
        lastWatch = watch;
        moveAngle = watch ? R_PointToAngle2(mo->x, mo->y, watch->soundorg.x, watch->soundorg.y) : mo->angle;
    }
    else
    {
        float tx, ty;
        Lookahead(x, y, &tx, &ty);
        moveAngle = R_PointToAngle2(mo->x, mo->y, (fixed_t)(tx * FRACUNIT), (fixed_t)(ty * FRACUNIT));
    }
    // Blocked for a second (usually a monster in the way): sidestep, a different side each time.
    if (!g_fighting && !holding && g_stuckTics > TICRATE)
    {
        if (g_stuckTics % TICRATE == 0)
            g_nudgeSign = -g_nudgeSign;
        moveAngle += g_nudgeSign * (ANG45 + ANG45 / 3);
    }

    // Shooting the target would set off a barrel whose blast reaches us: back away from it and
    // step aside for a clear shot (fire is held below until the shot is safe). Only for a while,
    // so a barrel can't hold the rider back for good.
    g_autofiring = pace / MPS_PER_RPM >= RIDE_AutofireRpm();
    int wantFire = g_target && ((touchDown && g_targetTapped) || (g_autofiring && P_CheckSight(mo, g_target)));
    int evading = 0;
    mobj_t *lit, *hurts;
    if (wantFire && !holding && gametic >= g_evadeRestUntil &&
        BarrelDangerTowards(mo, g_target, player->readyweapon, &lit, &hurts))
    {
        if (g_evadeTics++ == 0)
            LOGI("Ride: barrel in the line of fire, backing away");
        if (g_evadeTics > EVADE_MAX_TICS)
        {
            LOGI("Ride: still no safe shot; riding on");
            g_evadeTics = 0;
            g_evadeRestUntil = gametic + EVADE_REST_TICS;
        }
        else
        {
            float dx = (g_target->x - mo->x) / (float)FRACUNIT, dy = (g_target->y - mo->y) / (float)FRACUNIT;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < 1) d = 1;
            moveAngle = BarrelSteer(mo, lit, hurts, dx / d, dy / d);
            evading = 1;
        }
    }
    else
    {
        if (g_evadeTics)
            LOGI("Ride: %s after %.1f s", !g_target ? "target gone" : wantFire ? "clear shot" : "stopped firing",
                 g_evadeTics / (float)TICRATE);
        g_evadeTics = 0;
    }

    // Shots on their way: step out of their paths, even if the rider has stopped pedalling, but
    // not off a lift that is carrying us. Boss fights dodge in BossSteer.
    int dodging = 0;
    if (!g_fighting && !(holding && mo->subsector->sector->specialdata))
    {
        int fine = moveAngle >> ANGLETOFINESHIFT;
        int going = pace >= 0.2f && !holding;   // weigh the way we are going only if we are going
        float mx = going ? finecosine[fine] / (float)FRACUNIT : 0, my = going ? finesine[fine] / (float)FRACUNIT : 0;
        float dx, dy, score;
        int shots = ShotDodge(mo, finecosine[fine] / (float)FRACUNIT, finesine[fine] / (float)FRACUNIT, &dx, &dy);
        if (shots)
        {
            angle_t dodge = ClearHeading(mo, mx + dx, my + dy, &score);
            if (score > -2)
            {
                if (!g_dodging)
                    LOGI("Ride: dodging (%d threat%s)", shots, shots > 1 ? "s" : "");
                moveAngle = dodge;
                dodging = 1;
            }
        }
    }
    g_dodging = dodging;

    angle_t faceAngle = g_target ? R_PointToAngle2(mo->x, mo->y, g_target->x, g_target->y) : moveAngle;
    int turn = TurnTowards(mo->angle, faceAngle, g_target ? 1536 : 1024);

    float speedFrac = pace / FULL_PACE;
    if (speedFrac > 1) speedFrac = 1;
    if (pace < 0.2f || holding) speedFrac = 0;
    int speed = (int)(speedFrac * MAX_MOVE);
    if (dodging && speed < DODGE_MOVE)
        speed = DODGE_MOVE;

    // Not going anywhere (stopped pedalling, or waiting) and not dodging: brake against any
    // slide left over from a dodge, so a stopped rider stays where they stopped.
    int braking = 0;
    if (speed == 0 && !dodging && !g_fighting && !(holding && mo->subsector->sector->specialdata))
    {
        float vx = mo->momx / (float)FRACUNIT, vy = mo->momy / (float)FRACUNIT;
        float v = sqrtf(vx * vx + vy * vy);
        if (v > 1)
        {
            moveAngle = R_PointToAngle2(0, 0, -mo->momx, -mo->momy);
            speed = v * 8 > MAX_MOVE ? MAX_MOVE : (int)(v * 8);
            braking = 1;
        }
    }

    angle_t rel = moveAngle - (mo->angle + ((angle_t)turn << 16));
    int fine = rel >> ANGLETOFINESHIFT;
    int forward = FixedMul(speed << FRACBITS, finecosine[fine]) >> FRACBITS;
    int side = -(FixedMul(speed << FRACBITS, finesine[fine]) >> FRACBITS);

    if (RIDE_TRACE && gametic % TICRATE == 0 && g_routeState == ROUTE_OK)
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "Ride trace: at (%.0f,%.0f) progress %d/%d target %s speed %d stuck %d sector %d moving %d",
                            x, y, g_progress, g_routeLen, g_target ? "yes" : "no", speed, g_stuckTics,
                            SectorIndex(mo->subsector->sector), mo->subsector->sector->specialdata != NULL);

    cmd->forwardmove = (signed char)Clamp(forward, -MAX_MOVE, MAX_MOVE);
    cmd->sidemove = (signed char)Clamp(side, -MAX_MOVE, MAX_MOVE);
    cmd->angleturn = (short)turn;

    // Fire while the rider holds a monster, or at any monster in sight while pedalling at the
    // auto-fire cadence, once roughly facing it.
    cmd->buttons &= ~BT_ATTACK;
    if (wantFire)
    {
        int off = (int)(faceAngle - mo->angle);
        if (off < 0) off = -off;
        // Never a rocket at something inside its blast, nor a shot that sets off a barrel
        // next to us. The shot goes where we face, so check along that.
        int aim = mo->angle >> ANGLETOFINESHIFT;
        if (off < (int)(ANG45 / 4) && (player->readyweapon != wp_missile || RocketSafe(mo, 0)))
        {
            if (!BarrelDanger(mo, finecosine[aim] / (float)FRACUNIT, finesine[aim] / (float)FRACUNIT,
                              player->readyweapon, &lit, &hurts))
                cmd->buttons |= BT_ATTACK;
            else if (RIDE_COMBAT_LOG && gametic % (TICRATE / 2) == 0)
                LOGI("Ride: holding fire, barrel %.0f units ahead would blast us from %.0f",
                     sqrtf(Dist2(x, y, lit->x / (float)FRACUNIT, lit->y / (float)FRACUNIT)),
                     BlastDistance(hurts, mo));
        }
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
    // Pedalling but getting no further along the route (bumping back and forth against something):
    // treat it as stuck too.
    if (g_routeState == ROUTE_OK && g_progress > g_bestProgress)
    {
        g_bestProgress = g_progress;
        g_bestProgressTic = gametic;
    }
    if (speed == 0 || braking || waiting || g_fighting || evading || dodging)
        g_bestProgressTic = gametic;
    if (gametic - g_bestProgressTic > 2 * STUCK_TICS)
    {
        g_bestProgressTic = gametic;
        g_stuckTics = STUCK_TICS * 2 + 1;   // let the stuck handling below take it
        g_stuckX = mo->x;
        g_stuckY = mo->y;
    }
    if (speed > 0 && !braking && !g_fighting && !evading && !dodging && !holding)
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
            // The objective being worked towards (a detour is only a means to it).
            if (g_objKind == OBJ_DETOUR)
            {
                g_objKind = g_savedKind;
                g_objLine = g_savedLine;
                g_objKey = g_savedKey;
                g_objColor = g_savedColor;
            }
            int abandoned = 0;
            if (++g_objStucks >= 3 && (g_objKind == OBJ_KEY || g_objKind == OBJ_LINE))
            {
                AbandonObjective("stuck on the way");
                abandoned = 1;
            }
            LOGI("Ride: stuck at (%d,%d) sector %d floor %d ceil %d; heading for (%.0f,%.0f) sector %d "
                 "floor %d ceil %d tag %d moving %d; planning again",
                 mo->x >> FRACBITS, mo->y >> FRACBITS, SectorIndex(hs), hs->floorheight >> FRACBITS,
                 hs->ceilingheight >> FRACBITS, CellX(ahead), CellY(ahead), g_cellSector[ahead],
                 as->floorheight >> FRACBITS, as->ceilingheight >> FRACBITS, as->tag, as->specialdata != NULL);
            g_stuckTics = 0;
            g_routeState = ROUTE_NONE;
            if (!abandoned && !g_fighting && StartDetour(mo))
                g_routeState = ROUTE_OK;
        }
    }
}
