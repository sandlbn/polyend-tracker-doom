// polyend-doom -- MUS -> MIDI sequencer test (src/doom/pt_music.c).
//
// Plays every D_* music lump of a WAD to its end on a simulated clock and
// checks the MIDI stream: well-formed messages, every note-on matched by a
// note-off (or an all-notes-off), and a playing time that equals the sum of
// the score's delays at 140 Hz.
//
//   mus_test path/to/doom1.wad

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

static uint32_t nowMs;
uint32_t DG_GetTicksMs(void) { return nowMs; }

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

static int notesOn[16][128];
static unsigned long messages, noteOns;
static int badMessage;

void pt_midi_send(const uint8_t* msg, int len)
{
    ++messages;
    if (len < 1 || len > 3 || !(msg[0] & 0x80))
    {
        badMessage = 1;
        return;
    }
    for (int i = 1; i < len; ++i)
        if (msg[i] & 0x80)
            badMessage = 1;
    const int st = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    const int expect = st == 0xC0 || st == 0xD0 ? 2 : 3;
    if (len != expect)
        badMessage = 1;
    if (st == 0x90 && msg[2] > 0)
    {
        ++notesOn[ch][msg[1]];
        ++noteOns;
    }
    else if (st == 0x80 || (st == 0x90 && msg[2] == 0))
    {
        if (notesOn[ch][msg[1]] > 0)
            --notesOn[ch][msg[1]];
    }
    else if (st == 0xB0 && (msg[1] == 123 || msg[1] == 120))
    {
        memset(notesOn[ch], 0, sizeof(notesOn[ch]));
    }
}

extern music_module_t DG_music_module;

// Total score length in MUS ticks, decoded independently of the player.
static long ScoreTicks(const uint8_t* p, int len)
{
    const int scoreLen = p[4] | p[5] << 8, start = p[6] | p[7] << 8;
    const uint8_t* q = p + start;
    const uint8_t* end = start + scoreLen <= len ? q + scoreLen : p + len;
    static const int data[8] = {1, 1, 1, 1, 2, 0, 0, 0};
    long ticks = 0;
    while (q < end)
    {
        const uint8_t ev = *q++;
        const int type = ev >> 4 & 7;
        if (type == 1 && (q[0] & 0x80))
            q += 2;
        else
            q += data[type];
        if (type == 6)
            break;
        if (ev & 0x80)
        {
            long d = 0;
            uint8_t b;
            do
            {
                b = *q++;
                d = d << 7 | (b & 0x7F);
            } while (b & 0x80);
            ticks += d;
        }
    }
    return ticks;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: mus_test file.wad\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f)
    {
        fprintf(stderr, "mus_test: cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    uint8_t* wad = malloc((size_t)size);
    fseek(f, 0, SEEK_SET);
    if (fread(wad, 1, (size_t)size, f) != (size_t)size)
        return 2;
    fclose(f);

    const int numLumps = wad[4] | wad[5] << 8 | wad[6] << 16 | wad[7] << 24;
    const int dirOfs = wad[8] | wad[9] << 8 | wad[10] << 16 | wad[11] << 24;
    music_module_t* m = &DG_music_module;
    CHECK(m->Init(), "Init");
    int songs = 0;
    for (int i = 0; i < numLumps; ++i)
    {
        const uint8_t* e = wad + dirOfs + 16 * i;
        const int pos = e[0] | e[1] << 8 | e[2] << 16 | e[3] << 24;
        const int len = e[4] | e[5] << 8 | e[6] << 16 | e[7] << 24;
        char name[9] = {0};
        memcpy(name, e + 8, 8);
        if (strncmp(name, "D_", 2) != 0)
            continue;
        ++songs;
        memset(notesOn, 0, sizeof(notesOn));
        badMessage = 0;
        noteOns = 0;
        nowMs = 1000;
        void* h = m->RegisterSong(wad + pos, len);
        CHECK(h != NULL, "%s: not registered", name);
        if (!h)
            continue;
        m->PlaySong(h, false);
        const long ticks = ScoreTicks(wad + pos, len);
        const uint32_t expectMs = (uint32_t)(ticks * 1000 / 140);
        // Step a frame (~28 ms) at a time, as the game does.
        while (m->MusicIsPlaying() && nowMs < 1000 + expectMs + 5000)
        {
            nowMs += 28;
            m->Poll();
        }
        CHECK(!m->MusicIsPlaying(), "%s: still playing after %u ms (score %u ms)", name, nowMs - 1000, expectMs);
        const uint32_t played = nowMs - 1000;
        CHECK(played + 28 >= expectMs && played <= expectMs + 28 + 8, "%s: ended after %u ms, score is %u ms", name,
              played, expectMs);
        CHECK(!badMessage, "%s: malformed MIDI message", name);
        CHECK(noteOns > 0, "%s: no notes", name);
        int hanging = 0;
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < 128; ++n)
                hanging += notesOn[c][n];
        CHECK(hanging == 0, "%s: %d notes left on", name, hanging);
        m->UnRegisterSong(h);
    }
    CHECK(songs > 0, "no D_* lumps in %s", argv[1]);

    // Looping: the score restarts and keeps sending notes.
    for (int i = 0; i < numLumps; ++i)
    {
        const uint8_t* e = wad + dirOfs + 16 * i;
        if (memcmp(e + 8, "D_E1M1", 6) != 0)
            continue;
        const int pos = e[0] | e[1] << 8 | e[2] << 16 | e[3] << 24;
        const int len = e[4] | e[5] << 8 | e[6] << 16 | e[7] << 24;
        void* h = m->RegisterSong(wad + pos, len);
        m->PlaySong(h, true);
        const uint32_t scoreMs = (uint32_t)(ScoreTicks(wad + pos, len) * 1000 / 140);
        nowMs = 0;
        unsigned long before = 0;
        for (uint32_t t = 0; t < 3 * scoreMs; t += 28)
        {
            nowMs = t;
            m->Poll();
            if (t < scoreMs + 1000)
                before = noteOns;
        }
        CHECK(m->MusicIsPlaying(), "D_E1M1 loop stopped");
        CHECK(noteOns > before, "D_E1M1 loop sent no notes after the first pass");
        m->StopSong();
    }

    printf("mus_test: %d songs, %lu MIDI messages, %s\n", songs, messages, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
