// OpenTracker -- LMEM system cache for the SDRAM (sdram_cache.h). Register model:
// NXP K66 RM "Local Memory Controller" (PSCCR/PSCLCR/PSCSAR/PSCRMR at
// 0xE0082800). UNVERIFIED on hardware: the whole module is new in 2.0.0; its
// boot self-test decides whether the cache stays on (docs/HARDWARE_TESTING.md).
#include "sdram_cache.h"

#include <Arduino.h>
#include <DMAChannel.h>
#include <string.h>
#include "board.h"
#include "diag.h"

namespace ot {
namespace hal {
namespace cache {

namespace {
#define OT_LMEM_PSCCR  (*(volatile uint32_t*)0xE0082800u)
#define OT_LMEM_PSCLCR (*(volatile uint32_t*)0xE0082804u)
#define OT_LMEM_PSCSAR (*(volatile uint32_t*)0xE0082808u)
#define OT_LMEM_PSCRMR (*(volatile uint32_t*)0xE0082820u)
// PSCCR: ENCACHE(0) ENWRBUF(1) INVW0(24) PUSHW0(25) INVW1(26) PUSHW1(27) GO(31).
constexpr uint32_t CCR_ENCACHE = 1u << 0, CCR_INVW0 = 1u << 24, CCR_INVW1 = 1u << 26, CCR_GO = 1u << 31;
// PSCLCR: LCMD(25:24) 01 = invalidate, LADSEL(26) 1 = address in PSCSAR. PSCSAR: LGO(0).
constexpr uint32_t LCR_LCMD_INV = 1u << 24, LCR_LADSEL = 1u << 26, SAR_LGO = 1u << 0;

Status gStatus;

inline void dsb() { __asm__ volatile("dsb" ::: "memory"); }

bool waitClear(volatile uint32_t& reg, uint32_t bit) {
    for (uint32_t n = 0; n < 100000; n++)
        if (!(reg & bit)) return true;
    return false;
}

void hwInvalidateAll() {
    OT_LMEM_PSCCR = (OT_LMEM_PSCCR & CCR_ENCACHE) | CCR_GO | CCR_INVW0 | CCR_INVW1;
    waitClear(OT_LMEM_PSCCR, CCR_GO);
}

[[maybe_unused]] void hwEnable() {
    // K66 RM: change PSCRMR only while the cache is disabled.
    OT_LMEM_PSCCR = 0;
    dsb();
    OT_LMEM_PSCRMR = detail::sdramOnlyRmr(board::kSdramBase);
    OT_LMEM_PSCCR = CCR_GO | CCR_INVW0 | CCR_INVW1;
    waitClear(OT_LMEM_PSCCR, CCR_GO);
    OT_LMEM_PSCCR = CCR_ENCACHE;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    gStatus.enabled = true;
}

void hwDisable() {
    // Write-through: no dirty lines, so turning it off loses nothing.
    OT_LMEM_PSCCR = 0;
    dsb();
    OT_LMEM_PSCCR = CCR_GO | CCR_INVW0 | CCR_INVW1;
    waitClear(OT_LMEM_PSCCR, CCR_GO);
    OT_LMEM_PSCCR = 0;
    OT_LMEM_PSCRMR = 0;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    gStatus.enabled = false;
}

// eDMA memory-to-memory copy (software start, one major iteration, 32-bit
// beats), polled. False on timeout or a DMA error (crossbar cannot reach).
// UNVERIFIED: K66 crossbar lets the eDMA master (M2) reach the SDRAM controller.
bool dmaCopy(void* dst, const void* src, uint32_t bytes) {
    DMAChannel ch(false);
    ch.begin(true);
    ch.TCD->SADDR = src;
    ch.TCD->SOFF = 4;
    ch.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    ch.TCD->NBYTES_MLNO = bytes;
    ch.TCD->SLAST = 0;
    ch.TCD->DADDR = dst;
    ch.TCD->DOFF = 4;
    ch.TCD->CITER_ELINKNO = 1;
    ch.TCD->DLASTSGA = 0;
    ch.TCD->BITER_ELINKNO = 1;
    ch.TCD->CSR = 0;
    DMA_CERR = ch.channel;
    dsb();
    ch.triggerManual();
    const uint32_t t0 = millis();
    bool done = false;
    while (millis() - t0 < 20) {
        if (ch.TCD->CSR & DMA_TCD_CSR_DONE) { done = true; break; }
        if (DMA_ERR & (1u << ch.channel)) break;
    }
    const bool err = (DMA_ERR & (1u << ch.channel)) != 0;
    if (err) DMA_CERR = ch.channel;
    ch.disable();   // the destructor releases the channel
    dsb();
    return done && !err;
}

void fill(uint32_t* p, uint32_t words, uint32_t seed) {
    for (uint32_t i = 0; i < words; i++) p[i] = seed ^ (i * 0x9E3779B1u);
}
uint32_t mismatches(const volatile uint32_t* p, uint32_t words, uint32_t seed) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++)
        if (p[i] != (seed ^ (i * 0x9E3779B1u))) bad++;
    return bad;
}
uint32_t sumCycles(const volatile uint32_t* p, uint32_t words, uint32_t reps) {
    uint32_t s = 0;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (uint32_t r = 0; r < reps; r++)
        for (uint32_t i = 0; i < words; i++) s += p[i];
    const uint32_t c = ARM_DWT_CYCCNT - c0;
    __asm__ volatile("" ::"r"(s));
    return c;
}

constexpr uint32_t kBufBytes = 4096, kBufWords = kBufBytes / 4;
constexpr uint32_t kStreamBytes = 65536;
constexpr uint32_t kSeedA = 0xA5A50000u, kSeedB = 0x5A5A1111u, kSeedC = 0x3C3C2222u;

// Coherency test (cache on): X cached with A, eDMA writes B behind the cache,
// invalidate must make B visible (line path on 512 B, then whole cache); then
// the CPU writes C and the eDMA must read C (clean path).
[[maybe_unused]] void coherencyTest(uint32_t* x, uint32_t* y) {
    Status& s = gStatus;
    fill(y, kBufWords, kSeedB);
    fill(x, kBufWords, kSeedA);
    (void)mismatches(x, kBufWords, kSeedA);            // pull X into the cache
    if (!dmaCopy(x, y, kBufBytes)) { s.test = Test::Inconclusive; return; }
    s.staleSeen = mismatches(x, kBufWords, kSeedB) != 0;
    invalidate(x, 512);                                // line path (< threshold)
    uint32_t bad = mismatches(x, 512 / 4, kSeedB);
    if (bad) { s.test = Test::Failed; s.failStep = 1; s.failWords = bad; return; }
    invalidate(x, kBufBytes);                          // whole-cache path
    bad = mismatches(x, kBufWords, kSeedB);
    if (bad) { s.test = Test::Failed; s.failStep = 2; s.failWords = bad; return; }
    fill(x, kBufWords, kSeedC);
    cleanForDma(x, kBufBytes);
    (void)mismatches(y, kBufWords, kSeedB);            // Y cached with B
    if (!dmaCopy(y, x, kBufBytes)) { s.test = Test::Inconclusive; return; }
    invalidate(y, kBufBytes);
    bad = mismatches(y, kBufWords, kSeedC);
    if (bad) { s.test = Test::Failed; s.failStep = 3; s.failWords = bad; return; }
    s.test = Test::Ok;
}
}  // namespace

const char* testName(Test t) {
    switch (t) {
    case Test::NotRun: return "not-run";
    case Test::Ok: return "ok";
    case Test::Failed: return "FAILED";
    case Test::Inconclusive: return "inconclusive";   // eDMA could not reach the SDRAM
    case Test::BuildOff: return "build-off";          // OT_SDRAM_CACHE=0
    }
    return "?";
}

const Status& status() { return gStatus; }
bool sdramCached() { return gStatus.enabled; }

void disable() {
    if (gStatus.enabled) hwDisable();
}

void invalidateAll() {
    if (gStatus.enabled) hwInvalidateAll();
}

void invalidate(const void* p, uint32_t len) {
    if (!gStatus.enabled) return;
    const detail::LineRange r =
        detail::linesFor(reinterpret_cast<uint32_t>(p), len, board::kSdramBase, board::kSdramSize);
    if (!r.count) return;
    if (detail::useWholeCache(r.count * detail::kLineBytes)) { hwInvalidateAll(); return; }
    OT_LMEM_PSCLCR = LCR_LADSEL | LCR_LCMD_INV;
    for (uint32_t i = 0, a = r.first; i < r.count; i++, a += detail::kLineBytes) {
        OT_LMEM_PSCSAR = a | SAR_LGO;
        waitClear(OT_LMEM_PSCSAR, SAR_LGO);
    }
    dsb();
}

void cleanForDma(const void* p, uint32_t len) {
    (void)p;
    (void)len;
    // Write-through, write buffer off: memory is already current; make sure the
    // last stores have left the core before the DMA master starts.
    dsb();
}

void initSdram(uint8_t* scratch) {
    Status& s = gStatus;
    s = Status();
    uint32_t* x = reinterpret_cast<uint32_t*>(scratch);
    uint32_t* y = reinterpret_cast<uint32_t*>(scratch + kBufBytes);
    const uint32_t* stream = reinterpret_cast<const uint32_t*>(scratch + 2 * kBufBytes);

    // Cache off: does the eDMA reach the SDRAM at all, and the uncached speed.
    hwDisable();
    fill(y, kBufWords, kSeedB);
    fill(x, kBufWords, kSeedA);
    s.dmaReach = dmaCopy(x, y, kBufBytes) && mismatches(x, kBufWords, kSeedB) == 0;
    s.streamOff = sumCycles(stream, kStreamBytes / 4, 1);
    s.hotOff = sumCycles(x, kBufWords, 16);

#if OT_SDRAM_CACHE
    hwEnable();
    s.streamOn = sumCycles(stream, kStreamBytes / 4, 1);
    s.hotOn = sumCycles(x, kBufWords, 16);
    if (s.dmaReach) coherencyTest(x, y);
    else s.test = Test::Inconclusive;
    if (s.test == Test::Failed) hwDisable();
#else
    s.test = Test::BuildOff;
#endif
    diagLogf("cache: sdram %s, selftest %s%s (dma reach %d, stale seen %d, fail step %u words %lu)",
             s.enabled ? "write-through ON" : "OFF", testName(s.test),
             s.test == Test::Failed ? ", cache turned off" : "", int(s.dmaReach), int(s.staleSeen),
             unsigned(s.failStep), static_cast<unsigned long>(s.failWords));
    diagLogf("cache: sdram read cycles off -> on: 64 KB stream %lu -> %lu, 4 KB x16 hot %lu -> %lu",
             static_cast<unsigned long>(s.streamOff), static_cast<unsigned long>(s.streamOn),
             static_cast<unsigned long>(s.hotOff), static_cast<unsigned long>(s.hotOn));
}

}  // namespace cache
}  // namespace hal
}  // namespace ot
