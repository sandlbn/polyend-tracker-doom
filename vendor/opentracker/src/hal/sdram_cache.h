// OpenTracker -- LMEM system cache (PS) for the external SDRAM (docs/PLAN_2X.md
// performance workstream "LMEM system cache").
//
// K66 RM "Local Memory Controller": the system-bus cache (8 KB, 2-way) serves
// core accesses to 0x20000000..0xFFFFFFFF; PSCRMR gives each 256 MB region a mode
// (00/01 non-cacheable, 10 write-through, 11 write-back). OpenTracker sets only
// region 7 (0x70000000, the SDRAM at board::kSdramBase) to write-through and
// every other region to non-cacheable, so SRAM_U (DMA buffers, stack), the
// peripherals and the FlexBus stay uncached. The write buffer (PSCCR.ENWRBUF)
// stays off: every CPU store reaches the SDRAM before the next instruction
// that depends on it, so "clean before DMA reads SDRAM" is a DSB.
//
// Coherency rules for SDRAM buffers touched by a DMA master:
//   * after a DMA wrote SDRAM:   invalidate(p, len) before the CPU reads it;
//   * before a DMA reads SDRAM:  cleanForDma(p, len) (write-through: drain only).
// SRAM DMA buffers (dma_mem.h) need neither.
//
// Build flag OT_SDRAM_CACHE=0 (Makefile) leaves the cache off. With the flag on,
// initSdram() enables it after the SDRAM came up and runs a coherency self-test
// with the eDMA engine; a failed test turns the cache off again (logged).
#pragma once

#include <stdint.h>

#ifndef OT_SDRAM_CACHE
#define OT_SDRAM_CACHE 1
#endif

namespace ot {
namespace hal {
namespace cache {

enum class Test : uint8_t {
    NotRun = 0,      // initSdram() not called (or SDRAM failed)
    Ok,              // stale data seen before invalidate, fresh after; clean path ok
    Failed,          // incoherent after invalidate / clean -> cache turned off
    Inconclusive,    // eDMA did not reach the SDRAM (cache stays on; no eDMA->SDRAM user)
    BuildOff,        // OT_SDRAM_CACHE=0
};
const char* testName(Test t);

struct Status {
    bool enabled = false;        // PS cache on for the SDRAM region right now
    Test test = Test::NotRun;
    bool dmaReach = false;       // eDMA SDRAM->SDRAM copy worked with the cache off
    bool staleSeen = false;      // cached copy differed from memory before invalidate (cache active)
    uint32_t failWords = 0;      // mismatching words in the failed step
    uint8_t failStep = 0;        // 1 invalidate(line) 2 invalidate(all) 3 clean
    // DWT cycles to sum 64 KB of SDRAM (streaming) and 4 KB x 16 (hot set),
    // cache off and on: the measured effect of the cache.
    uint32_t streamOff = 0, streamOn = 0, hotOff = 0, hotOn = 0;
};

// After SDRAM init + quick test (destroys `scratch`: >= 80 KB of tested SDRAM).
void initSdram(uint8_t* scratch);
bool sdramCached();
// Turns the cache off (invalidated first); idempotent.
void disable();
const Status& status();

// Cache maintenance for SDRAM buffers (no-ops when the cache is off or the range
// is outside the SDRAM). Safe from the main loop; not used from ISRs.
void invalidate(const void* p, uint32_t len);
void cleanForDma(const void* p, uint32_t len);
void invalidateAll();

// ---- pure helpers (host-tested in tests/sdram_cache_test.cpp) ---------------------
namespace detail {
// LMEM PSCRMR/PCCRMR: region n (0..15, 256 MB each) in bits [31-2n : 30-2n].
constexpr uint32_t kModeNonCacheable = 0, kModeWriteThrough = 2, kModeWriteBack = 3;
constexpr uint32_t rmrField(uint32_t region, uint32_t mode) { return (mode & 3u) << (30u - 2u * region); }
constexpr uint32_t regionOf(uint32_t addr) { return addr >> 28; }
// Only the region holding `sdramBase` is write-through.
constexpr uint32_t sdramOnlyRmr(uint32_t sdramBase) { return rmrField(regionOf(sdramBase), kModeWriteThrough); }

constexpr uint32_t kLineBytes = 16;          // K66 LMEM: 16-byte lines (4 words)
constexpr uint32_t kCacheBytes = 8192;
// Above this many bytes one whole-cache invalidate (write-through: nothing dirty
// to lose) is cheaper than line-by-line commands.
constexpr uint32_t kWholeCacheThreshold = 2048;

struct LineRange { uint32_t first, count; };
// Lines covering [addr, addr+len) clipped to [base, base+size); count 0 if none.
constexpr LineRange linesFor(uint32_t addr, uint32_t len, uint32_t base, uint32_t size) {
    const uint64_t lo0 = addr, hi0 = uint64_t(addr) + len;
    const uint64_t lo = lo0 < base ? base : lo0;
    const uint64_t hi = hi0 > uint64_t(base) + size ? uint64_t(base) + size : hi0;
    if (len == 0 || hi <= lo) return LineRange{0, 0};
    const uint64_t first = lo & ~uint64_t(kLineBytes - 1);
    const uint64_t last = (hi - 1) & ~uint64_t(kLineBytes - 1);
    return LineRange{uint32_t(first), uint32_t((last - first) / kLineBytes + 1)};
}
constexpr bool useWholeCache(uint32_t len) { return len > kWholeCacheThreshold; }
}  // namespace detail

}  // namespace cache
}  // namespace hal
}  // namespace ot
