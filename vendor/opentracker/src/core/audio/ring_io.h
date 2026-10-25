// Bulk int16 copies between the effect rings / sample pool (SDRAM on the
// device) and small working buffers (SRAM). OpenTracker CPU optimisation,
// no spec behaviour: an uncached SDRAM access costs tens of CPU cycles, so
// the DSP loops read and write contiguous spans once per block with 32-bit
// accesses instead of one 16-bit access per tap per sample.
#pragma once

#include <cstdint>

#include "audio/dsp.h"
#include "fastrun.h"

// Benchmark builds (tests/audio_bench.cpp) count slow-memory accesses
// (32-bit words or single int16 reads) to estimate SDRAM stall time.
#ifdef OT_AUDIO_SDRAM_STATS
#define OT_SDRAM_ACCESS(n) (::ot::audio::gSdramAccesses += uint32_t(n))
#else
#define OT_SDRAM_ACCESS(n) ((void)0)
#endif

namespace ot {
namespace audio {

#ifdef OT_AUDIO_SDRAM_STATS
extern uint32_t gSdramAccesses;
#endif

typedef uint32_t __attribute__((may_alias)) u32_alias;

// Copy n32 words (n32 % 4 == 0 not required) between word-aligned buffers;
// returns the advanced pointers. On the M4 the bulk moves 16 bytes per
// LDM/STM pair (the registers are named, so the list order is fixed).
inline void wordCopy(u32_alias*& d, const u32_alias*& s, uint32_t n32) {
#if defined(__arm__) && defined(__thumb2__)
    while (n32 >= 8) {
        register uint32_t r4 __asm__("r4");
        register uint32_t r5 __asm__("r5");
        register uint32_t r6 __asm__("r6");
        register uint32_t r7 __asm__("r7");
        register uint32_t r8 __asm__("r8");
        register uint32_t r9 __asm__("r9");
        register uint32_t r10 __asm__("r10");
        register uint32_t r11 __asm__("r11");
        __asm__ volatile(
            "ldmia %[s]!, {%[a], %[b], %[c], %[e], %[f], %[g], %[h], %[k]}\n\t"
            "stmia %[d]!, {%[a], %[b], %[c], %[e], %[f], %[g], %[h], %[k]}"
            : [s] "+r"(s), [d] "+r"(d), [a] "=r"(r4), [b] "=r"(r5), [c] "=r"(r6), [e] "=r"(r7), [f] "=r"(r8),
              [g] "=r"(r9), [h] "=r"(r10), [k] "=r"(r11)
            :
            : "memory");
        n32 -= 8;
    }
    if (n32 >= 4) {
        register uint32_t r4 __asm__("r4");
        register uint32_t r5 __asm__("r5");
        register uint32_t r6 __asm__("r6");
        register uint32_t r7 __asm__("r7");
        __asm__ volatile("ldmia %[s]!, {%[a], %[b], %[c], %[e]}\n\tstmia %[d]!, {%[a], %[b], %[c], %[e]}"
                         : [s] "+r"(s), [d] "+r"(d), [a] "=r"(r4), [b] "=r"(r5), [c] "=r"(r6), [e] "=r"(r7)
                         :
                         : "memory");
        n32 -= 4;
    }
#else
    while (n32 >= 4) {
        OT_NO_LIBCALL();
        const uint32_t w0 = s[0], w1 = s[1], w2 = s[2], w3 = s[3];
        d[0] = w0;
        d[1] = w1;
        d[2] = w2;
        d[3] = w3;
        s += 4;
        d += 4;
        n32 -= 4;
    }
#endif
    while (n32) {
        OT_NO_LIBCALL();
        *d++ = *s++;
        --n32;
    }
}

// dst[0..n) = src[0..n), src in slow memory (any int16 alignment).
OT_FASTRUN_INLINE(spanIn) inline void spanIn(const int16_t* src, int16_t* __restrict dst, uint32_t n) {
    if (n && (reinterpret_cast<uintptr_t>(src) & 2u)) {
        *dst++ = *src++;
        --n;
    }
    const u32_alias* s = reinterpret_cast<const u32_alias*>(src);
    if (!(reinterpret_cast<uintptr_t>(dst) & 2u)) {   // both word aligned: word copy
        u32_alias* d = reinterpret_cast<u32_alias*>(dst);
        wordCopy(d, s, n / 2u);
        if (n & 1u) *reinterpret_cast<int16_t*>(d) = *reinterpret_cast<const int16_t*>(s);
        return;
    }
    while (n >= 8) {
        OT_NO_LIBCALL();
        const uint32_t w0 = s[0], w1 = s[1], w2 = s[2], w3 = s[3];
        dst[0] = int16_t(w0);
        dst[1] = int16_t(w0 >> 16);
        dst[2] = int16_t(w1);
        dst[3] = int16_t(w1 >> 16);
        dst[4] = int16_t(w2);
        dst[5] = int16_t(w2 >> 16);
        dst[6] = int16_t(w3);
        dst[7] = int16_t(w3 >> 16);
        s += 4;
        dst += 8;
        n -= 8;
    }
    while (n >= 2) {
        OT_NO_LIBCALL();
        const uint32_t w = *s++;
        dst[0] = int16_t(w);
        dst[1] = int16_t(w >> 16);
        dst += 2;
        n -= 2;
    }
    if (n) *dst = *reinterpret_cast<const int16_t*>(s);
}

// dst[0..n) = src[0..n), dst in slow memory (any int16 alignment).
inline void spanOut(int16_t* dst, const int16_t* __restrict src, uint32_t n) {
    if (n && (reinterpret_cast<uintptr_t>(dst) & 2u)) {
        *dst++ = *src++;
        --n;
    }
    u32_alias* d = reinterpret_cast<u32_alias*>(dst);
    if (!(reinterpret_cast<uintptr_t>(src) & 2u)) {   // both word aligned: word copy
        const u32_alias* s = reinterpret_cast<const u32_alias*>(src);
        wordCopy(d, s, n / 2u);
        if (n & 1u) *reinterpret_cast<int16_t*>(d) = *reinterpret_cast<const int16_t*>(s);
        return;
    }
    while (n >= 8) {
        OT_NO_LIBCALL();
        d[0] = uint32_t(uint16_t(src[0])) | (uint32_t(uint16_t(src[1])) << 16);
        d[1] = uint32_t(uint16_t(src[2])) | (uint32_t(uint16_t(src[3])) << 16);
        d[2] = uint32_t(uint16_t(src[4])) | (uint32_t(uint16_t(src[5])) << 16);
        d[3] = uint32_t(uint16_t(src[6])) | (uint32_t(uint16_t(src[7])) << 16);
        d += 4;
        src += 8;
        n -= 8;
    }
    while (n >= 2) {
        OT_NO_LIBCALL();
        *d++ = uint32_t(uint16_t(src[0])) | (uint32_t(uint16_t(src[1])) << 16);
        src += 2;
        n -= 2;
    }
    if (n) *reinterpret_cast<int16_t*>(d) = *src;
}

// Zero dst[0..n) in slow memory.
inline void spanZero(int16_t* dst, uint32_t n) {
    if (n && (reinterpret_cast<uintptr_t>(dst) & 2u)) {
        *dst++ = 0;
        --n;
    }
    u32_alias* d = reinterpret_cast<u32_alias*>(dst);
    while (n >= 8) {
        OT_NO_LIBCALL();
        d[0] = 0;
        d[1] = 0;
        d[2] = 0;
        d[3] = 0;
        d += 4;
        n -= 8;
    }
    while (n >= 2) {
        OT_NO_LIBCALL();
        *d++ = 0;
        n -= 2;
    }
    if (n) *reinterpret_cast<int16_t*>(d) = 0;
}

// Ring versions: positions (pos + k) & mask, k = 0..n-1 (ring length mask + 1).
OT_FASTRUN_INLINE(ringIn) inline void ringIn(const int16_t* ring, uint32_t mask, uint32_t pos, int16_t* __restrict dst, uint32_t n) {
    pos &= mask;
    while (n) {
        uint32_t seg = mask + 1u - pos;
        if (seg > n) seg = n;
        OT_SDRAM_ACCESS((seg + 2u) / 2u);
        spanIn(ring + pos, dst, seg);
        dst += seg;
        n -= seg;
        pos = (pos + seg) & mask;
    }
}
OT_FASTRUN_INLINE(ringOut) inline void ringOut(int16_t* ring, uint32_t mask, uint32_t pos, const int16_t* __restrict src, uint32_t n) {
    pos &= mask;
    while (n) {
        uint32_t seg = mask + 1u - pos;
        if (seg > n) seg = n;
        OT_SDRAM_ACCESS((seg + 2u) / 2u);
        spanOut(ring + pos, src, seg);
        src += seg;
        n -= seg;
        pos = (pos + seg) & mask;
    }
}
inline void ringZero(int16_t* ring, uint32_t mask, uint32_t pos, uint32_t n) {
    pos &= mask;
    while (n) {
        uint32_t seg = mask + 1u - pos;
        if (seg > n) seg = n;
        OT_SDRAM_ACCESS((seg + 2u) / 2u);
        spanZero(ring + pos, seg);
        n -= seg;
        pos = (pos + seg) & mask;
    }
}

}  // namespace audio
}  // namespace ot
