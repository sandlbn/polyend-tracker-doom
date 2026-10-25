// polyend-doom -- doomgeneric sound module (DG_sound_module): the sound
// effects mixer, pulled by the platform's audio callback through pt_mix().
//
// Effects stay in their cached WAD lumps (PU_STATIC, never purged): 8-bit
// unsigned DMX samples, usually 11025 Hz, stepped to the output rate without
// interpolation (the crunch of the original). Channel records are shared with
// the audio interrupt: the main loop clears `active`, fills the record, then
// sets `active` again, and the interrupt skips inactive channels.

#include <stdio.h>
#include <string.h>

#include "deh_str.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "pt.h"

#define MIX_CHANNELS 16

typedef struct {
    const uint8_t* data;   // unsigned 8-bit samples
    uint32_t length;       // samples
    uint32_t step;         // 16.16 source samples per output sample
} pt_sfx_t;

typedef struct {
    const uint8_t* volatile data;
    volatile uint32_t length16;   // length << 16
    volatile uint32_t pos16;      // 16.16 position, advanced by the interrupt
    volatile uint32_t step;
    volatile int16_t gainL, gainR;   // 0..127
    volatile uint8_t active;
} mix_channel_t;

static mix_channel_t channels[MIX_CHANNELS];
static boolean useSfxPrefix;

// Config variables i_sound.c binds; they belong to the SDL module we replace.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

// ---- mixer (audio interrupt) -------------------------------------------------------

void pt_mix(int16_t* out, uint32_t frames)
{
    // 32-bit accumulators: up to 16 channels x 128 x 127 fits easily.
    int32_t accL[128], accR[128];
    while (frames)
    {
        const uint32_t n = frames < 128 ? frames : 128;
        memset(accL, 0, n * sizeof(int32_t));
        memset(accR, 0, n * sizeof(int32_t));
        for (int c = 0; c < MIX_CHANNELS; ++c)
        {
            mix_channel_t* ch = &channels[c];
            if (!ch->active)
                continue;
            const uint8_t* d = ch->data;
            const uint32_t len16 = ch->length16, step = ch->step;
            const int32_t gl = ch->gainL, gr = ch->gainR;
            uint32_t pos = ch->pos16;
            uint32_t i = 0;
            for (; i < n && pos < len16; ++i, pos += step)
            {
                const int32_t s = (int32_t)d[pos >> 16] - 128;
                accL[i] += s * gl;
                accR[i] += s * gr;
            }
            ch->pos16 = pos;
            if (pos >= len16)
                ch->active = 0;
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            int32_t l = accL[i], r = accR[i];
            l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
            r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
            out[2 * i] = (int16_t)l;
            out[2 * i + 1] = (int16_t)r;
        }
        out += 2 * n;
        frames -= n;
    }
}

// ---- sound module (main loop) ------------------------------------------------------

static snddevice_t soundDevices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

static boolean Init(boolean use_sfx_prefix)
{
    useSfxPrefix = use_sfx_prefix;
    for (int c = 0; c < MIX_CHANNELS; ++c)
        channels[c].active = 0;
    return true;
}

static void Shutdown(void)
{
    for (int c = 0; c < MIX_CHANNELS; ++c)
        channels[c].active = 0;
}

static int GetSfxLumpNum(sfxinfo_t* sfx)
{
    char name[9];
    if (sfx->link != NULL)
        sfx = sfx->link;
    if (useSfxPrefix)
        snprintf(name, sizeof(name), "ds%.6s", DEH_String(sfx->name));
    else
        snprintf(name, sizeof(name), "%s", DEH_String(sfx->name));
    return W_GetNumForName(name);
}

// Parse a DMX sound lump once; NULL if it is not one.
static pt_sfx_t* Load(sfxinfo_t* sfx)
{
    if (sfx->driver_data != NULL)
        return sfx->driver_data;
    const int lump = sfx->lumpnum;
    const uint32_t lumplen = (uint32_t)W_LumpLength(lump);
    const uint8_t* data = W_CacheLumpNum(lump, PU_STATIC);
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
        return NULL;
    const uint32_t rate = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
    const uint32_t length = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) |
                            ((uint32_t)data[7] << 24);
    if (length > lumplen - 8 || length <= 48 || rate == 0)
        return NULL;
    pt_sfx_t* s = Z_Malloc(sizeof(pt_sfx_t), PU_STATIC, NULL);
    // As Chocolate Doom: DMX skips the first and last 16 bytes of the lump.
    s->data = data + 16;
    s->length = length - 32;
    // 16.16 positions: at most 65535 samples (5.9 s at 11025 Hz; every IWAD
    // effect is shorter).
    if (s->length > 0xFFFF)
        s->length = 0xFFFF;
    s->step = (uint32_t)(((uint64_t)rate << 16) / PT_MIX_RATE);
    sfx->driver_data = s;
    return s;
}

static void SetGains(mix_channel_t* ch, int vol, int sep)
{
    // vol 0..127, sep 0 (left) .. 254 (right), 128 = centre (equal gains).
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    ch->gainL = (int16_t)(vol * (256 - sep) / 256);
    ch->gainR = (int16_t)(vol * sep / 256);
}

static void UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return;
    SetGains(&channels[channel], vol, sep);
}

static int StartSound(sfxinfo_t* sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return -1;
    const pt_sfx_t* s = Load(sfx);
    if (s == NULL)
        return -1;
    mix_channel_t* ch = &channels[channel];
    ch->active = 0;
    barrier();
    ch->data = s->data;
    ch->length16 = s->length << 16;
    ch->pos16 = 0;
    ch->step = s->step;
    SetGains(ch, vol, sep);
    barrier();
    ch->active = 1;
    return channel;
}

static void StopSound(int channel)
{
    if (channel >= 0 && channel < MIX_CHANNELS)
        channels[channel].active = 0;
}

static boolean SoundIsPlaying(int channel)
{
    return channel >= 0 && channel < MIX_CHANNELS && channels[channel].active;
}

static void Update(void) {}

static void CacheSounds(sfxinfo_t* sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;   // loaded on first use
}

sound_module_t DG_sound_module = {
    soundDevices,
    sizeof(soundDevices) / sizeof(*soundDevices),
    Init,
    Shutdown,
    GetSfxLumpNum,
    Update,
    UpdateSoundParams,
    StartSound,
    StopSound,
    SoundIsPlaying,
    CacheSounds,
};
