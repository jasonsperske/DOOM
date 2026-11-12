// Ride mode: bike-driven movement along a planned route (ride.c).
#ifndef RIDE_H
#define RIDE_H

#include "d_ticcmd.h"

// UI thread
void RIDE_SetEnabled(int enabled);
void RIDE_SetBike(int live, float pace, float resistance);
void RIDE_SetTouch(int down, float u, float v);   // u, v: 0..1 across the 320x200 frame

// Read from the UI thread; a stale value is harmless.
int RIDE_IsActive(void);
float RIDE_Power(void);    // pedal power 0..1
int RIDE_Weapon(void);
int RIDE_Objective(void);   // what the route heads for: see RIDE_Objective in ride.c      // weapontype_t chosen by pedal power, or -1 outside ride mode
int RIDE_HasTarget(void);

// Engine hooks (g_game.c, p_setup.c)
void RIDE_BuildTiccmd(ticcmd_t* cmd);
void RIDE_LevelLoaded(void);

#endif
