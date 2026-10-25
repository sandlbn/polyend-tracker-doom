// src/hal/fast_mem.c (OP-1: the device's word-wise memcpy / memmove / memset)
// against byte-loop references, for every small alignment / length mix and
// overlapping moves in both directions. Built here under other names (the
// host keeps its libc); the M4 benchmark runs the same check on the ARM
// code paths (LDM/STM blocks, unaligned word loads).
#include <cstdint>
#include <cstring>
#include <vector>

#include "test.h"

#define memcpy ot_fast_memcpy
#define memmove ot_fast_memmove
#define memset ot_fast_memset
#include "../src/hal/fast_mem.c"
#undef memcpy
#undef memmove
#undef memset

namespace {
void fill(std::vector<uint8_t>& v, uint32_t seed) {
    for (auto& b : v) {
        seed = seed * 1664525u + 1013904223u;
        b = uint8_t(seed >> 24);
    }
}
}  // namespace

TEST(fast_mem_matches_reference) {
    constexpr size_t kBuf = 2048;
    int bad = 0, cases = 0;
    std::vector<uint8_t> a(kBuf), b(kBuf), src(kBuf);
    for (size_t n : {0u, 1u, 3u, 4u, 7u, 8u, 9u, 15u, 16u, 31u, 32u, 33u, 63u, 64u, 100u, 255u, 256u, 600u}) {
        for (size_t da = 0; da < 4; ++da)
            for (size_t sa = 0; sa < 4; ++sa) {
                // memcpy
                fill(src, uint32_t(n * 131 + da * 7 + sa));
                fill(a, 99);
                b = a;
                void* r = ot_fast_memcpy(a.data() + 64 + da, src.data() + 32 + sa, n);
                for (size_t i = 0; i < n; ++i) b[64 + da + i] = src[32 + sa + i];
                bad += a != b || r != a.data() + 64 + da;
                // memset
                fill(a, uint32_t(n + da));
                b = a;
                r = ot_fast_memset(a.data() + 64 + da, int(0x1A5 + sa), n);
                for (size_t i = 0; i < n; ++i) b[64 + da + i] = uint8_t(0xA5 + sa);
                bad += a != b || r != a.data() + 64 + da;
                // memmove, overlapping both ways and disjoint
                for (int shift : {-37, -5, -4, -1, 0, 1, 3, 4, 8, 29, 300, -300}) {
                    fill(a, uint32_t(n * 3 + da + sa + shift));
                    b = a;
                    const size_t so = 400 + sa, d0 = size_t(int(400 + da) + shift);
                    r = ot_fast_memmove(a.data() + d0, a.data() + so, n);
                    std::vector<uint8_t> tmp(b.begin() + so, b.begin() + so + n);
                    for (size_t i = 0; i < n; ++i) b[d0 + i] = tmp[i];
                    bad += a != b || r != a.data() + d0;
                    ++cases;
                }
                cases += 2;
            }
    }
    std::printf("  %d mem* cases, %d differ\n", cases, bad);
    CHECK_EQ(bad, 0);
}
