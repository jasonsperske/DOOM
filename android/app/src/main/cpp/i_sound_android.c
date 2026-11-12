// Android replacement for linuxdoom-1.10/i_sound.c: sound effects through OpenSL ES.
//
// OpenSL ES is the native audio API on Android 7.0 (AAudio arrived in 8.0). The mixer runs
// in OpenSL's buffer-queue callback, so the game thread only starts, stops and adjusts
// channels under a lock; I_UpdateSound and I_SubmitSound have nothing to do.
//
// Music is in i_music_android.c.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "doom_android.h"

#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"

#define OUTPUT_RATE   44100
#define BUFFER_FRAMES 1024          // about 23 ms
#define NUM_BUFFERS   3
#define MIX_CHANNELS  16

typedef struct
{
    const byte* samples;   // 8-bit unsigned PCM
    unsigned length;       // in samples
    unsigned rate;
} sfxdata_t;

typedef struct
{
    const sfxdata_t* sfx;
    uint32_t pos;          // 16.16 fixed point index into samples
    uint32_t step;         // 16.16 source samples per output frame
    int leftvol, rightvol; // 0..127
    int handle;
    int id;
} channel_t;

static sfxdata_t g_sfx[NUMSFX];
static channel_t g_channels[MIX_CHANNELS];
static int g_nextHandle = 1;
static pthread_mutex_t g_mixLock = PTHREAD_MUTEX_INITIALIZER;

static SLObjectItf g_engineObj, g_mixObj, g_playerObj;
static SLPlayItf g_play;
static SLAndroidSimpleBufferQueueItf g_queue;
static int16_t g_buffers[NUM_BUFFERS][BUFFER_FRAMES * 2];
static int g_nextBuffer;

// ---------------------------------------------------------------------------
// Sound effect data

static void LoadSfx(int id)
{
    char name[16];
    sfxinfo_t* info = &S_sfx[id];
    int lump;

    // Linked sounds (e.g. chaingun -> pistol) play another sound's data.
    if (info->link)
        info = info->link;

    snprintf(name, sizeof(name), "ds%s", info->name);
    lump = W_CheckNumForName(name);
    if (lump == -1)
        lump = W_GetNumForName("dspistol");   // DOOM II sounds are missing from DOOM 1 WADs

    int size = W_LumpLength(lump);
    const byte* data = W_CacheLumpNum(lump, PU_STATIC);
    if (size < 8)
        return;

    // DMX header: format (3), sample rate, sample count, then unsigned 8-bit samples.
    unsigned rate = data[2] | (data[3] << 8);
    unsigned count = data[4] | (data[5] << 8) | (data[6] << 16) | ((unsigned)data[7] << 24);
    if (count > (unsigned)(size - 8))
        count = size - 8;

    g_sfx[id].samples = data + 8;
    g_sfx[id].length = count;
    g_sfx[id].rate = rate ? rate : 11025;
    S_sfx[id].data = (void*)data;   // s_sound.c only checks that this is set
}

int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

// ---------------------------------------------------------------------------
// Mixer

static void Mix(int16_t* out, int frames)
{
    pthread_mutex_lock(&g_mixLock);
    for (int f = 0; f < frames; f++)
    {
        int left = 0, right = 0;
        for (int c = 0; c < MIX_CHANNELS; c++)
        {
            channel_t* ch = &g_channels[c];
            if (!ch->sfx)
                continue;
            unsigned index = ch->pos >> 16;
            if (index >= ch->sfx->length)
            {
                ch->sfx = NULL;
                continue;
            }
            int s = (int)ch->sfx->samples[index] - 128;
            left += s * ch->leftvol;
            right += s * ch->rightvol;
            ch->pos += ch->step;
        }
        if (left > 32767) left = 32767; else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767; else if (right < -32768) right = -32768;
        out[f * 2] = (int16_t)left;
        out[f * 2 + 1] = (int16_t)right;
    }
    pthread_mutex_unlock(&g_mixLock);
}

static void QueueCallback(SLAndroidSimpleBufferQueueItf queue, void* context)
{
    int16_t* buf = g_buffers[g_nextBuffer];
    g_nextBuffer = (g_nextBuffer + 1) % NUM_BUFFERS;
    Mix(buf, BUFFER_FRAMES);
    (*queue)->Enqueue(queue, buf, sizeof(g_buffers[0]));
}

// Stereo split as in linuxdoom: sep is 1..255 with 128 centred, vol is 0..127.
static void SetChannelParams(channel_t* ch, int vol, int sep)
{
    sep += 1;
    ch->leftvol = vol - ((vol * sep * sep) >> 16);
    sep -= 257;
    ch->rightvol = vol - ((vol * sep * sep) >> 16);
    if (ch->leftvol < 0) ch->leftvol = 0;
    if (ch->rightvol < 0) ch->rightvol = 0;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int handle;
    channel_t* slot = NULL;

    if (id <= 0 || id >= NUMSFX || !g_sfx[id].samples)
        return -1;

    pthread_mutex_lock(&g_mixLock);
    // Reuse a free channel, else the one started longest ago.
    for (int c = 0; c < MIX_CHANNELS; c++)
    {
        channel_t* ch = &g_channels[c];
        if (!ch->sfx) { slot = ch; break; }
        if (!slot || ch->handle < slot->handle)
            slot = ch;
    }
    handle = g_nextHandle++;
    slot->sfx = &g_sfx[id];
    slot->pos = 0;
    slot->step = (uint32_t)(((uint64_t)g_sfx[id].rate << 16) / OUTPUT_RATE);
    slot->handle = handle;
    slot->id = id;
    SetChannelParams(slot, vol, sep);
    pthread_mutex_unlock(&g_mixLock);
    return handle;
}

static channel_t* FindChannel(int handle)
{
    for (int c = 0; c < MIX_CHANNELS; c++)
        if (g_channels[c].sfx && g_channels[c].handle == handle)
            return &g_channels[c];
    return NULL;
}

void I_StopSound(int handle)
{
    pthread_mutex_lock(&g_mixLock);
    channel_t* ch = FindChannel(handle);
    if (ch)
        ch->sfx = NULL;
    pthread_mutex_unlock(&g_mixLock);
}

int I_SoundIsPlaying(int handle)
{
    pthread_mutex_lock(&g_mixLock);
    int playing = FindChannel(handle) != NULL;
    pthread_mutex_unlock(&g_mixLock);
    return playing;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    pthread_mutex_lock(&g_mixLock);
    channel_t* ch = FindChannel(handle);
    if (ch)
        SetChannelParams(ch, vol, sep);
    pthread_mutex_unlock(&g_mixLock);
}

void I_SetChannels(void)
{
}

void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;   // s_sound.c already scales each sound by it
}

void I_UpdateSound(void)
{
}

void I_SubmitSound(void)
{
}

// ---------------------------------------------------------------------------
// OpenSL ES

static int StartOutput(void)
{
    SLEngineItf engine;

    if (slCreateEngine(&g_engineObj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) return 0;
    if ((*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return 0;
    if ((*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS) return 0;

    if ((*engine)->CreateOutputMix(engine, &g_mixObj, 0, NULL, NULL) != SL_RESULT_SUCCESS) return 0;
    if ((*g_mixObj)->Realize(g_mixObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return 0;

    SLDataLocator_AndroidSimpleBufferQueue queueLoc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NUM_BUFFERS };
    SLDataFormat_PCM pcm = {
        SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource source = { &queueLoc, &pcm };
    SLDataLocator_OutputMix mixLoc = { SL_DATALOCATOR_OUTPUTMIX, g_mixObj };
    SLDataSink sink = { &mixLoc, NULL };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
    const SLboolean required[] = { SL_BOOLEAN_TRUE };

    if ((*engine)->CreateAudioPlayer(engine, &g_playerObj, &source, &sink, 1, ids, required) != SL_RESULT_SUCCESS) return 0;
    if ((*g_playerObj)->Realize(g_playerObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return 0;
    if ((*g_playerObj)->GetInterface(g_playerObj, SL_IID_PLAY, &g_play) != SL_RESULT_SUCCESS) return 0;
    if ((*g_playerObj)->GetInterface(g_playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_queue) != SL_RESULT_SUCCESS) return 0;
    if ((*g_queue)->RegisterCallback(g_queue, QueueCallback, NULL) != SL_RESULT_SUCCESS) return 0;

    // Prime every buffer with silence; each completion then mixes the next one.
    for (int i = 0; i < NUM_BUFFERS; i++)
        QueueCallback(g_queue, NULL);
    return (*g_play)->SetPlayState(g_play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS;
}

void I_InitSound(void)
{
    for (int i = 1; i < NUMSFX; i++)
        LoadSfx(i);
    if (StartOutput())
        printf("I_InitSound: OpenSL ES output at %d Hz\n", OUTPUT_RATE);
    else
    {
        printf("I_InitSound: could not open audio output; continuing without sound\n");
        g_play = NULL;
    }
}

void DA_SoundPause(int paused)
{
    if (g_play)
        (*g_play)->SetPlayState(g_play, paused ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);
}

void I_ShutdownSound(void)
{
    if (g_playerObj)
        (*g_playerObj)->Destroy(g_playerObj);
    if (g_mixObj)
        (*g_mixObj)->Destroy(g_mixObj);
    if (g_engineObj)
        (*g_engineObj)->Destroy(g_engineObj);
    g_playerObj = g_mixObj = g_engineObj = NULL;
    g_play = NULL;
}
