// Music: DOOM's MUS lumps converted to standard MIDI files and played by Android's built-in
// MIDI synthesiser (MediaPlayer, see MusicPlayer.kt).
//
// The conversion follows the MUS format: events of note on/off, pitch bend, controllers and
// system events on 16 channels at 140 ticks a second, MUS channel 15 being percussion (MIDI
// channel 10). Songs that are already MIDI (some PWADs) are played as they are.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doom_android.h"

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"

// ---------------------------------------------------------------------------
// MUS -> MIDI

typedef struct
{
    byte* data;
    int size, cap;
} buf_t;

static void Put(buf_t* b, int byteValue)
{
    if (b->size == b->cap)
    {
        b->cap = b->cap ? b->cap * 2 : 16384;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->size++] = (byte)byteValue;
}

static void PutVarLen(buf_t* b, uint32_t value)
{
    byte bytes[5];
    int n = 0;
    bytes[n++] = value & 0x7f;
    while ((value >>= 7))
        bytes[n++] = 0x80 | (value & 0x7f);
    while (n--)
        Put(b, bytes[n]);
}

static void Put32(buf_t* b, uint32_t v)
{
    Put(b, v >> 24); Put(b, v >> 16); Put(b, v >> 8); Put(b, v);
}

// MUS controller numbers 0-9 (0 is program change) and system events 10-14, as MIDI controllers.
static const int musToMidiController[15] = {
    0, 0, 1, 7, 10, 11, 91, 93, 64, 67,   // program, bank, modulation, volume, pan, expression,
                                          // reverb, chorus, sustain, soft pedal
    120, 123, 126, 127, 121               // sounds off, notes off, mono, poly, reset controllers
};

// Converts a MUS lump; returns a malloc'd MIDI file and its size, or NULL if it is not MUS.
static byte* MusToMidi(const byte* mus, int* outSize)
{
    if (memcmp(mus, "MUS\x1a", 4) != 0)
        return NULL;

    int scoreLen = mus[4] | (mus[5] << 8);
    int scoreStart = mus[6] | (mus[7] << 8);
    const byte* p = mus + scoreStart;
    const byte* end = p + scoreLen;

    int midiChannel[16];          // MUS channel -> MIDI channel, -1 until first used
    int noteVolume[16];
    int nextChannel = 0;
    for (int i = 0; i < 16; i++)
    {
        midiChannel[i] = -1;
        noteVolume[i] = 127;
    }

    buf_t track = { 0 };
    // Tempo: 500000 us per quarter note with 70 ticks per quarter = 140 ticks a second.
    Put(&track, 0); Put(&track, 0xff); Put(&track, 0x51); Put(&track, 3);
    Put(&track, 0x07); Put(&track, 0xa1); Put(&track, 0x20);

    uint32_t delay = 0;
    int done = 0;
    while (!done && p < end)
    {
        int desc = *p++;
        int last = desc & 0x80;
        int type = (desc >> 4) & 7;
        int ch = desc & 15;

        if (midiChannel[ch] < 0)
        {
            if (ch == 15)
                midiChannel[ch] = 9;               // percussion
            else
            {
                if (nextChannel == 9)
                    nextChannel++;
                midiChannel[ch] = nextChannel++ & 15;
            }
            // Start each channel at full volume, as DOOM's own drivers did.
            PutVarLen(&track, delay);
            delay = 0;
            Put(&track, 0xb0 | midiChannel[ch]); Put(&track, 7); Put(&track, 127);
        }
        int mc = midiChannel[ch];

        switch (type)
        {
          case 0:   // release note
            PutVarLen(&track, delay); delay = 0;
            Put(&track, 0x80 | mc); Put(&track, *p++ & 0x7f); Put(&track, 0);
            break;

          case 1:   // play note, with a new volume if the top bit is set
          {
            int note = *p++;
            if (note & 0x80)
                noteVolume[ch] = *p++ & 0x7f;
            PutVarLen(&track, delay); delay = 0;
            Put(&track, 0x90 | mc); Put(&track, note & 0x7f); Put(&track, noteVolume[ch]);
            break;
          }

          case 2:   // pitch bend: 0..255, 128 centred
          {
            int bend = *p++ * 64;
            PutVarLen(&track, delay); delay = 0;
            Put(&track, 0xe0 | mc); Put(&track, bend & 0x7f); Put(&track, (bend >> 7) & 0x7f);
            break;
          }

          case 3:   // system event
          {
            int ctrl = *p++;
            if (ctrl >= 10 && ctrl <= 14)
            {
                PutVarLen(&track, delay); delay = 0;
                Put(&track, 0xb0 | mc); Put(&track, musToMidiController[ctrl]); Put(&track, 0);
            }
            break;
          }

          case 4:   // controller
          {
            int ctrl = *p++;
            int value = *p++ & 0x7f;
            PutVarLen(&track, delay); delay = 0;
            if (ctrl == 0)
            {
                Put(&track, 0xc0 | mc); Put(&track, value);
            }
            else if (ctrl < 10)
            {
                Put(&track, 0xb0 | mc); Put(&track, musToMidiController[ctrl]); Put(&track, value);
            }
            else
            {
                // Unknown: keep the timing with a harmless event.
                Put(&track, 0xb0 | mc); Put(&track, 7); Put(&track, 127);
            }
            break;
          }

          case 5:   // end of measure
            break;

          case 6:   // score end
            done = 1;
            break;

          default:  // unused; nothing sensible to do
            done = 1;
            break;
        }

        if (last && !done)
        {
            uint32_t t = 0;
            int b;
            do
            {
                b = *p++;
                t = (t << 7) | (b & 0x7f);
            } while ((b & 0x80) && p < end);
            delay += t;
        }
    }

    PutVarLen(&track, delay);
    Put(&track, 0xff); Put(&track, 0x2f); Put(&track, 0);

    buf_t midi = { 0 };
    const char* header = "MThd";
    for (int i = 0; i < 4; i++) Put(&midi, header[i]);
    Put32(&midi, 6);
    Put(&midi, 0); Put(&midi, 0);     // format 0
    Put(&midi, 0); Put(&midi, 1);     // one track
    Put(&midi, 0); Put(&midi, 70);    // 70 ticks per quarter note
    const char* trackHeader = "MTrk";
    for (int i = 0; i < 4; i++) Put(&midi, trackHeader[i]);
    Put32(&midi, track.size);
    for (int i = 0; i < track.size; i++)
        Put(&midi, track.data[i]);
    free(track.data);

    *outSize = midi.size;
    return midi.data;
}

// ---------------------------------------------------------------------------
// Songs

#define MAX_SONGS 4

typedef struct
{
    byte* midi;
    int size;
} song_t;

static song_t g_songs[MAX_SONGS];
static int g_nextFile;

void I_InitMusic(void) {}

void I_ShutdownMusic(void)
{
    DA_MusicStop();
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    float v = volume / 15.0f;   // the menu's 0..15
    DA_MusicVolume(v > 1 ? 1 : v);
}

int I_RegisterSong(void* data)
{
    const byte* lump = data;
    byte* midi = NULL;
    int size = 0;

    if (!memcmp(lump, "MThd", 4))
    {
        // Already MIDI: its length is the sum of its chunks.
        const byte* p = lump;
        size = 14;
        p += 14;
        while (!memcmp(p, "MTrk", 4))
        {
            int len = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
            size += 8 + len;
            p += 8 + len;
        }
        midi = malloc(size);
        memcpy(midi, lump, size);
    }
    else
        midi = MusToMidi(lump, &size);

    if (!midi)
    {
        printf("I_RegisterSong: not a MUS or MIDI lump\n");
        return 0;
    }

    for (int h = 1; h < MAX_SONGS; h++)
        if (!g_songs[h].midi)
        {
            g_songs[h].midi = midi;
            g_songs[h].size = size;
            return h;
        }
    free(midi);
    return 0;
}

void I_UnRegisterSong(int handle)
{
    if (handle <= 0 || handle >= MAX_SONGS)
        return;
    free(g_songs[handle].midi);
    g_songs[handle].midi = NULL;
}

void I_PlaySong(int handle, int looping)
{
    char path[512];
    const char* home = getenv("HOME");
    if (handle <= 0 || handle >= MAX_SONGS || !g_songs[handle].midi || !home)
        return;

    // Alternate files so the player never reads a file being rewritten.
    snprintf(path, sizeof(path), "%s/music%d.mid", home, g_nextFile);
    g_nextFile ^= 1;
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(g_songs[handle].midi, 1, g_songs[handle].size, f);
    fclose(f);

    DA_MusicPlay(path, looping);
}

void I_StopSong(int handle)
{
    DA_MusicStop();
}

void I_PauseSong(int handle)
{
    DA_MusicPause(1);
}

void I_ResumeSong(int handle)
{
    DA_MusicPause(0);
}

int I_QrySongPlaying(int handle)
{
    return handle > 0 && handle < MAX_SONGS && g_songs[handle].midi != NULL;
}
