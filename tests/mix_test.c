// polyend-doom -- sound effects mixer test (src/doom/pt_sound.c): DMX lump
// parsing, rate stepping, panning, channel end, stop and clipping.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

#include "pt.h"

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fputc('\n', stderr);                          \
            ++failures;                                   \
        }                                                 \
    } while (0)

// ---- the engine functions pt_sound.c uses -----------------------------------------
static uint8_t lump[8 + 16 + 1000 + 16];
int W_LumpLength(unsigned int lump_) { (void)lump_; return sizeof(lump); }
void* W_CacheLumpNum(int lump_, int tag) { (void)lump_; (void)tag; return lump; }
int W_GetNumForName(char* name) { (void)name; return 0; }
void* Z_Malloc(int size, int tag, void* user) { (void)tag; (void)user; return malloc((size_t)size); }
char* DEH_String(char* s) { return s; }

extern sound_module_t DG_sound_module;

int main(void)
{
    // DMX header: format 3, 11025 Hz, 1032 samples (1000 + 2 x 16 padding).
    lump[0] = 3;
    lump[2] = 11025 & 0xFF;
    lump[3] = 11025 >> 8;
    const uint32_t n = 1000 + 32;
    lump[4] = n & 0xFF;
    lump[5] = n >> 8;
    // lump[8..15] and the last 16 bytes are the padding DMX skips (left 0);
    // the 1000 played samples are full positive.
    memset(lump + 16, 255, 1000);

    sound_module_t* s = &DG_sound_module;
    CHECK(s->Init(true), "Init");
    sfxinfo_t sfx = {0};
    strcpy(sfx.name, "pistol");

    // Centre, full volume: both sides equal, 127 * 63 per sample.
    CHECK(s->StartSound(&sfx, 0, 127, 128) == 0, "StartSound");
    CHECK(s->SoundIsPlaying(0), "playing");
    int16_t out[2 * 128];
    pt_mix(out, 128);
    CHECK(out[0] == out[1], "centre: L %d R %d", out[0], out[1]);
    CHECK(out[0] == 127 * (127 * 128 / 256), "centre level %d", out[0]);

    // 1000 samples at 11025 Hz last 4000 output frames at 44100 Hz (+/- 1).
    int frames = 128;
    while (s->SoundIsPlaying(0) && frames < 10000)
    {
        pt_mix(out, 128);
        frames += 128;
    }
    CHECK(!s->SoundIsPlaying(0), "ended");
    CHECK(frames >= 4000 && frames <= 4000 + 128, "length %d frames", frames);
    pt_mix(out, 128);
    CHECK(out[0] == 0 && out[255] == 0, "silence after the end");

    // Hard left: right silent.
    s->StartSound(&sfx, 1, 127, 0);
    pt_mix(out, 4);
    CHECK(out[0] > 0 && out[1] == 0, "left: L %d R %d", out[0], out[1]);
    // Stop.
    s->StopSound(1);
    pt_mix(out, 4);
    CHECK(out[0] == 0 && out[1] == 0, "stopped");

    // Eight loud channels clip instead of wrapping.
    for (int c = 0; c < 8; ++c)
        s->StartSound(&sfx, c, 127, 128);
    pt_mix(out, 4);
    CHECK(out[0] == 32767 && out[1] == 32767, "clip: %d %d", out[0], out[1]);

    // Not a DMX lump: refused.
    sfxinfo_t bad = {0};
    lump[0] = 1;
    CHECK(s->StartSound(&bad, 9, 127, 128) == -1, "non-DMX lump played");

    printf("mix_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
