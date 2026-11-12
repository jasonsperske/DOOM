// Android replacement for linuxdoom-1.10/i_system.c.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doom_android.h"

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

// Zone heap. The original 6 MB was sized for 32-bit structures; 64-bit ones are larger.
int mb_used = 32;

void I_Tactile(int on, int off, int total)
{
}

ticcmd_t emptycmd;
ticcmd_t* I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte* I_ZoneBase(int* size)
{
    *size = mb_used * 1024 * 1024;
    return (byte*)malloc(*size);
}

//
// I_GetTime: tics (1/35 s) since the first call, not counting time spent paused.
//
int I_GetTime(void)
{
    static long long base;
    struct timespec ts;
    long long ms;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - DA_PausedMillis();
    if (!base)
        base = ms;
    return (int)((ms - base) * TICRATE / 1000);
}

void I_Init(void)
{
    I_InitSound();
}

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    DA_Quit();
}

void I_WaitVBL(int count)
{
    usleep(count * (1000000 / 70));
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte* I_AllocLow(int length)
{
    byte* mem = (byte*)malloc(length);
    memset(mem, 0, length);
    return mem;
}

extern boolean demorecording;

void I_Error(char* error, ...)
{
    static int recursive;
    char message[512];
    va_list argptr;

    va_start(argptr, error);
    vsnprintf(message, sizeof(message), error, argptr);
    va_end(argptr);

    // A shutdown step that fails calls I_Error again; report the first error only.
    if (!recursive)
    {
        recursive = 1;
        if (demorecording)
            G_CheckDemoStatus();
        D_QuitNetGame();
        I_ShutdownGraphics();
    }
    DA_FatalError(message);
}
