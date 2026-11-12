// Ride mode: bike-driven movement along a planned route (ride.c).
#ifndef RIDE_H
#define RIDE_H

#include "d_ticcmd.h"

// UI thread
void RIDE_SetEnabled(int enabled);
void RIDE_SetAutofireRpm(float rpm);   // 0: by skill level
void RIDE_SetBike(int live, float pace, float resistance);
void RIDE_SetTouch(int down, float u, float v);   // u, v: 0..1 across the 320x200 frame

// Read from the UI thread; a stale value is harmless.
int RIDE_IsActive(void);
float RIDE_Power(void);    // pedal power 0..1
int RIDE_Weapon(void);
int RIDE_Objective(void);
float RIDE_AutofireRpm(void);   // the cadence that switches auto-fire on
int RIDE_Autofiring(void);      // pedalling at or above it   // what the route heads for: see RIDE_Objective in ride.c      // weapontype_t chosen by pedal power, or -1 outside ride mode
int RIDE_HasTarget(void);

// Engine hooks (g_game.c, p_setup.c)
void RIDE_BuildTiccmd(ticcmd_t* cmd);
void RIDE_LevelLoaded(void);
void RIDE_NewGame(void);          // G_InitNew: start the episode totals afresh
void RIDE_LevelCompleted(void);   // G_DoCompleted: add the level's kills, items and secrets

// Totals since the game started, shown after the episode's closing text (f_finale.c).
typedef struct
{
    float meters;           // distance pedalled, from the bike's pace
    float ridingSeconds;    // time spent pedalling
    float resistance;       // average resistance while pedalling, 0..1
    int seconds;            // time in the levels completed
    int levels;
    int kills, maxKills;
    int items, maxItems;
    int secrets, maxSecrets;
} ridestats_t;
void RIDE_EpisodeStats(ridestats_t* out);

#endif
