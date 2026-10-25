// Prints the wavetable pitch tables and grain windows as computed by the
// device toolchain (newlib libm) under QEMU, as raw bit patterns;
// bench_m4/tables_inc.py turns them into src/core/audio/audio_tables_arm.inc.
// make -C tests audio-tables (see audio/audio_tables.h).
#include <cstdio>
#include <cstring>

#include "audio/audio_tables.h"

using namespace ot::audio;

static float wt[kWavetableSizes][kPitchTableSize];
static uint16_t win[3][kGrainWindowLen];

int main() {
    computeConstTables(wt, win);
    for (int k = 0; k < kWavetableSizes; ++k)
        for (int i = 0; i < kPitchTableSize; ++i) {
            uint32_t b;
            std::memcpy(&b, &wt[k][i], 4);
            std::printf("W %d %d %08lx\n", k, i, (unsigned long)b);
        }
    for (int s = 0; s < 3; ++s)
        for (int i = 0; i < kGrainWindowLen; ++i) std::printf("G %d %d %u\n", s, i, unsigned(win[s][i]));
    std::printf("END\n");
    return 0;
}
