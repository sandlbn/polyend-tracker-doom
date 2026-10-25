// polyend-doom -- doomgeneric music module (DG_music_module): MUS lumps
// played as MIDI on the Tracker's MIDI outputs (DIN + USB) for an external
// General MIDI synth. No synthesis on the device.
//
// The MUS score is stepped from Poll() (I_UpdateSound, once per displayed
// frame) at 140 ticks per second; events map onto MIDI as Chocolate Doom's
// mus2mid does (MUS channel 15 = MIDI percussion channel 10).

#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

#include "doomgeneric.h"
#include "pt.h"

#define MUS_RATE 140
#define MUS_CHANNELS 16

typedef struct {
    const uint8_t* score;   // first event
    const uint8_t* end;     // end of the lump
} song_t;

static song_t song;                // the one registered song
static const song_t* playing;      // NULL = stopped
static boolean looping, paused;
static const uint8_t* cursor;
static uint32_t nextTick;          // MUS tick of the event at cursor
static uint32_t startMs, pauseMs;  // time base (DG_GetTicksMs) of tick 0
static int musicVolume = 127;
static uint8_t chanVolume[MUS_CHANNELS];   // last MUS volume controller per channel
static uint8_t noteVelocity[MUS_CHANNELS]; // last "play note" volume per channel

static snddevice_t musicDevices[] = {
    SNDDEVICE_GENMIDI, SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32, SNDDEVICE_ADLIB,
};

static int MidiChannel(int mus)
{
    if (mus == 15)
        return 9;
    return mus >= 9 ? mus + 1 : mus;
}

static void Send3(uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t m[3] = {a, b, c};
    pt_midi_send(m, 3);
}

static void Send2(uint8_t a, uint8_t b)
{
    const uint8_t m[2] = {a, b};
    pt_midi_send(m, 2);
}

static void SendChannelVolume(int mus)
{
    const int v = chanVolume[mus] * musicVolume / 127;
    Send3((uint8_t)(0xB0 | MidiChannel(mus)), 7, (uint8_t)v);
}

static void AllNotesOff(void)
{
    for (int c = 0; c < 16; ++c)
        Send3((uint8_t)(0xB0 | c), 123, 0);
}

static void ResetChannels(void)
{
    for (int mus = 0; mus < MUS_CHANNELS; ++mus)
    {
        const uint8_t st = (uint8_t)(0xB0 | MidiChannel(mus));
        Send3(st, 121, 0);   // reset all controllers
        chanVolume[mus] = 127;
        noteVelocity[mus] = 127;
        SendChannelVolume(mus);
        Send2((uint8_t)(0xC0 | MidiChannel(mus)), 0);
    }
}

// Variable-length delay after an event with the "last" bit; false at the end.
static boolean ReadDelay(uint32_t* ticks)
{
    uint32_t t = 0;
    uint8_t b;
    do
    {
        if (cursor >= playing->end)
            return false;
        b = *cursor++;
        t = (t << 7) | (b & 0x7Fu);
    } while (b & 0x80);
    *ticks = t;
    return true;
}

// One event at the cursor. False when the score ended (and does not loop).
static boolean Step(void)
{
    static const uint8_t controllerMap[10] = {0, 0, 1, 7, 10, 11, 91, 93, 64, 67};
    static const uint8_t systemMap[5] = {120, 123, 126, 127, 121};
    if (cursor >= playing->end)
        return false;
    const uint8_t ev = *cursor++;
    const int mus = ev & 0x0F;
    const uint8_t ch = (uint8_t)MidiChannel(mus);
    static const uint8_t dataBytes[8] = {1, 1, 1, 1, 2, 0, 0, 0};
    if (cursor + dataBytes[(ev >> 4) & 7] > playing->end)
        return false;
    switch ((ev >> 4) & 7)
    {
    case 0:   // release note
        Send3((uint8_t)(0x80 | ch), *cursor++ & 0x7F, 0);
        break;
    case 1:   // play note [volume]
    {
        const uint8_t n = *cursor++;
        if (n & 0x80)
        {
            if (cursor >= playing->end)
                return false;
            noteVelocity[mus] = *cursor++ & 0x7F;
        }
        Send3((uint8_t)(0x90 | ch), n & 0x7F, noteVelocity[mus]);
        break;
    }
    case 2:   // pitch bend, 0..255 with 128 = centre
    {
        const unsigned bend = (unsigned)*cursor++ << 6;
        Send3((uint8_t)(0xE0 | ch), bend & 0x7F, (bend >> 7) & 0x7F);
        break;
    }
    case 3:   // system event
    {
        const uint8_t c = *cursor++;
        if (c >= 10 && c <= 14)
            Send3((uint8_t)(0xB0 | ch), systemMap[c - 10], 0);
        break;
    }
    case 4:   // controller
    {
        const uint8_t c = *cursor++;
        uint8_t v = *cursor++;
        if (v > 127)
            v = 127;
        if (c == 0)
            Send2((uint8_t)(0xC0 | ch), v);
        else if (c == 3)
        {
            chanVolume[mus] = v;
            SendChannelVolume(mus);
        }
        else if (c < 10)
            Send3((uint8_t)(0xB0 | ch), controllerMap[c], v);
        break;
    }
    case 5:   // end of measure
        break;
    case 6:   // score end
        if (!looping)
            return false;
        AllNotesOff();
        cursor = playing->score;
        return true;   // the loop restarts at the next tick, no delay byte
    default:   // 7: unused, no data
        break;
    }
    if (ev & 0x80)
    {
        uint32_t d;
        if (!ReadDelay(&d))
            return false;
        nextTick += d;
    }
    return true;
}

static boolean Init(void)
{
    playing = NULL;
    return true;
}

static void StopSong(void)
{
    if (playing != NULL)
        AllNotesOff();
    playing = NULL;
}

static void Shutdown(void)
{
    StopSong();
}

static void SetMusicVolume(int volume)
{
    musicVolume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    if (playing == NULL)
        return;
    for (int mus = 0; mus < MUS_CHANNELS; ++mus)
        SendChannelVolume(mus);
}

static void PauseMusic(void)
{
    if (playing == NULL || paused)
        return;
    paused = true;
    pauseMs = DG_GetTicksMs();
    AllNotesOff();
}

static void ResumeMusic(void)
{
    if (!paused)
        return;
    paused = false;
    startMs += DG_GetTicksMs() - pauseMs;
}

static void* RegisterSong(void* data, int len)
{
    // MUS header: "MUS" 0x1A, score length, score start (16-bit LE) ...
    const uint8_t* p = data;
    if (len < 16 || memcmp(p, "MUS\x1a", 4) != 0)
        return NULL;   // MIDI lumps (PWADs) are not played
    const uint32_t scoreLen = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
    const uint32_t scoreStart = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
    if (scoreStart >= (uint32_t)len)
        return NULL;
    StopSong();
    song.score = p + scoreStart;
    song.end = scoreStart + scoreLen <= (uint32_t)len ? song.score + scoreLen : p + len;
    return &song;
}

static void UnRegisterSong(void* handle)
{
    if (handle == playing)
        StopSong();
}

static void PlaySong(void* handle, boolean loop)
{
    StopSong();
    if (handle == NULL)
        return;
    playing = handle;
    looping = loop;
    paused = false;
    cursor = playing->score;
    nextTick = 0;
    startMs = DG_GetTicksMs();
    ResetChannels();
}

static boolean MusicIsPlaying(void)
{
    return playing != NULL;
}

static void Poll(void)
{
    if (playing == NULL || paused)
        return;
    const uint32_t now = (DG_GetTicksMs() - startMs) * MUS_RATE / 1000u;
    // Bounded per call so a corrupt score cannot hang the frame.
    for (int budget = 2048; budget > 0 && nextTick <= now; --budget)
    {
        if (!Step())
        {
            StopSong();
            return;
        }
        if (cursor == playing->score && nextTick != 0)
        {
            // Looped: restart the time base.
            nextTick = 0;
            startMs = DG_GetTicksMs();
            return;
        }
    }
}

music_module_t DG_music_module = {
    musicDevices,
    sizeof(musicDevices) / sizeof(*musicDevices),
    Init,
    Shutdown,
    SetMusicVolume,
    PauseMusic,
    ResumeMusic,
    RegisterSong,
    UnRegisterSong,
    PlaySong,
    StopSong,
    MusicIsPlaying,
    Poll,
};
