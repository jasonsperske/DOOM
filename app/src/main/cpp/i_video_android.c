// Android replacement for linuxdoom-1.10/i_video.c.
//
// DOOM draws a 320x200 8-bit paletted frame into screens[0]. I_FinishUpdate converts it
// through the palette and scales it (nearest neighbour, so pixels stay sharp) straight
// into the SurfaceView's buffer. The Java side sizes that view to 4:3, the shape DOOM was
// drawn for on a CRT, which on the bike's 1920x1080 panel is 1440x1080 centred.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doom_android.h"

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "d_main.h"
#include "doomdef.h"
#include "i_video.h"

static uint32_t g_palette[256];   // RGBX_8888 as little-endian words: 0xFFBBGGRR

static int g_mapWidth;            // buffer width g_xmap was built for
static int* g_xmap;               // destination column -> source column
static uint32_t* g_row;           // one converted, scaled row

void I_InitGraphics(void)
{
}

void I_ShutdownGraphics(void)
{
}

void I_StartFrame(void)
{
    DA_WaitWhilePaused();
}

void I_StartTic(void)
{
    DA_DrainInput();
}

void I_UpdateNoBlit(void)
{
}

static void EnsureScaleTables(int width)
{
    if (width == g_mapWidth)
        return;
    free(g_xmap);
    free(g_row);
    g_xmap = malloc(width * sizeof(int));
    g_row = malloc(width * sizeof(uint32_t));
    for (int x = 0; x < width; x++)
        g_xmap[x] = x * SCREENWIDTH / width;
    g_mapWidth = width;
}

void I_FinishUpdate(void)
{
    ANativeWindow_Buffer buffer;
    ANativeWindow* window = DA_LockWindow();
    if (!window)
        return;

    if (ANativeWindow_lock(window, &buffer, NULL) == 0)
    {
        int width = buffer.width;
        int height = buffer.height;
        uint32_t* dest = (uint32_t*)buffer.bits;
        const byte* src = screens[0];

        EnsureScaleTables(width);

        int destY = 0;
        for (int y = 0; y < SCREENHEIGHT && destY < height; y++, src += SCREENWIDTH)
        {
            int endY = (y + 1) * height / SCREENHEIGHT;
            if (endY <= destY)
                continue;

            for (int x = 0; x < width; x++)
                g_row[x] = g_palette[src[g_xmap[x]]];

            for (; destY < endY; destY++)
                memcpy(dest + (size_t)destY * buffer.stride, g_row, width * sizeof(uint32_t));
        }
        ANativeWindow_unlockAndPost(window);
    }
    DA_UnlockWindow();
}

void I_ReadScreen(byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte* palette)
{
    for (int i = 0; i < 256; i++)
    {
        uint32_t r = gammatable[usegamma][*palette++];
        uint32_t g = gammatable[usegamma][*palette++];
        uint32_t b = gammatable[usegamma][*palette++];
        g_palette[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}
