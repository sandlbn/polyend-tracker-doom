// 2.x performance workstream, HAL pure helpers: LMEM region mode word and line
// ranges (src/hal/sdram_cache.h), the SD/cache telemetry "io" line and the
// report hook (src/hal/telemetry.h), ADMA2 direct-target rule (src/hal/sdhc.h).
#include "test.h"

#include <cstring>
#include <string>

#include "hal/diag_log.cpp"    // platform independent; not part of libcore
#include "hal/telemetry.cpp"
#include "hal/sdram_cache.h"
#include "hal/sdhc.h"

using namespace ot::hal::cache::detail;

TEST(cache_rmr_sdram_only) {
    // Region 7 (0x70000000) = bits 17:16; write-through = 10b.
    CHECK(regionOf(0x70000000u) == 7);
    CHECK(sdramOnlyRmr(0x70000000u) == 0x00020000u);
    CHECK(rmrField(0, kModeWriteThrough) == 0x80000000u);
    CHECK(rmrField(15, kModeWriteBack) == 0x3u);
    // SRAM_U (region 2) and the peripherals (region 4) stay non-cacheable.
    CHECK(((sdramOnlyRmr(0x70000000u) >> (30 - 2 * 2)) & 3u) == kModeNonCacheable);
    CHECK(((sdramOnlyRmr(0x70000000u) >> (30 - 2 * 4)) & 3u) == kModeNonCacheable);
}

TEST(cache_line_ranges) {
    const uint32_t base = 0x70000000u, size = 0x01000000u;
    LineRange r = linesFor(base, 16, base, size);
    CHECK(r.first == base && r.count == 1);
    r = linesFor(base + 4, 16, base, size);              // straddles two lines
    CHECK(r.first == base && r.count == 2);
    r = linesFor(base + 0x100, 512, base, size);
    CHECK(r.first == base + 0x100 && r.count == 32);
    r = linesFor(base, 0, base, size);
    CHECK(r.count == 0);
    r = linesFor(0x20000000u, 4096, base, size);         // SRAM: nothing to do
    CHECK(r.count == 0);
    r = linesFor(base + size - 8, 64, base, size);       // clipped at the SDRAM end
    CHECK(r.first == base + size - 16 && r.count == 1);
    r = linesFor(base - 8, 32, base, size);              // clipped at the start
    CHECK(r.first == base && r.count == 2);
    CHECK(!useWholeCache(2048));
    CHECK(useWholeCache(2064));
}

TEST(sd_direct_rule_sdram) {
    using ot::hal::sdhc::detail::dmaDirectOk;
    const uint32_t base = 0x70000000u, size = 0x01000000u;
    CHECK(!dmaDirectOk(base, 32768, false, base, size));   // not until the check passed
    CHECK(dmaDirectOk(base, 32768, true, base, size));
    CHECK(dmaDirectOk(base + 4, 4096, true, base, size));
    CHECK(!dmaDirectOk(base + 2, 4096, true, base, size)); // ADMA2 needs word alignment
    CHECK(!dmaDirectOk(base + size - 512, 1024, true, base, size));
}

TEST(sd_chunk_plan_bank_alignment) {
    using ot::hal::sdhc::detail::planChunk;
    const uint32_t base = 0x70000000u, size = 0x01000000u;
    // Word-aligned SDRAM destination: direct, up to one ADMA2 table (512 blocks).
    auto c = planChunk(base + 0x1000, 1000, 512, 16, true, base, size);
    CHECK(c.direct && c.blocks == 512);
    c = planChunk(base + 0x1000, 40, 512, 16, true, base, size);
    CHECK(c.direct && c.blocks == 40);
    // Sample-bank pointer (int16, only 2-byte aligned): every sector of it stays
    // 2-byte aligned -> the whole transfer bounces in bounce-buffer chunks.
    for (uint32_t off = 0; off < 4; off++) {
        const uint32_t a = base + 0x2002 + off * 512;
        c = planChunk(a, 100, 512, 16, true, base, size);
        CHECK(!c.direct && c.blocks == 16);
    }
    c = planChunk(base + 0x2002, 3, 512, 16, true, base, size);
    CHECK(!c.direct && c.blocks == 3);
    // Odd address, and SDRAM before the direct check passed: bounce.
    c = planChunk(base + 0x3001, 8, 512, 16, true, base, size);
    CHECK(!c.direct && c.blocks == 8);
    c = planChunk(base + 0x1000, 64, 512, 16, false, base, size);
    CHECK(!c.direct && c.blocks == 16);
    // SRAM_U word-aligned (e.g. FatFs window): direct regardless of the SDRAM flag.
    c = planChunk(0x20001000u, 1, 512, 16, false, base, size);
    CHECK(c.direct && c.blocks == 1);
}

TEST(io_line_format) {
    using namespace ot::diag;
    IoSample s;
    s.windowMs = 5000;
    s.sdCmds = 42;
    s.sdReadBytes = 5u * 1024 * 1024;
    s.sdWriteBytes = 10240;
    s.sdDirect = 160;
    s.sdBounced = 3;
    s.cacheOn = true;
    s.cacheTest = "ok";
    s.sdDirectRead = true;
    char b[256];
    const size_t n = formatIo(b, sizeof b, s);
    CHECK(n == std::strlen(b));
    CHECK(std::string(b) ==
          "telemetry: io sd cmds 42 read 5242880 B (1024 KB/s) write 10240 B (2 KB/s) direct 160 bounced 3"
          " | cache sdram on (selftest ok) | sd->sdram dma read direct write bounced");
    CHECK(bytesPerSecond(1000, 0) == 0);
    CHECK(bytesPerSecond(5000, 5000) == 1000);
    char small[16];
    formatIo(small, sizeof small, s);                   // truncates, stays terminated
    CHECK(std::strlen(small) == sizeof small - 1);
}

TEST(load_line_format) {
    ot::JobProfile p;
    p.op = 2;
    p.running = true;
    p.passes = 40;
    p.units = 90;
    p.totalUs = 300000;
    p.maxPassUs = 9000;
    p.maxPassStage = 3;
    p.maxPassPhase = 1;
    p.maxUnitUs = 4000;
    p.maxUnitStage = 3;
    p.readUs = 120000;
    p.reads = 30;
    p.bytesRead = 3u * 1024 * 1024;
    char b[300];
    ot::diag::formatLoad(b, sizeof b, p);
    CHECK(std::string(b) ==
          "telemetry: load op 2 run 1 passes 40 units 90 total 300000 us max pass 9000 us (stage 3/1)"
          " max unit 4000 us (stage 3) | open 0 us n0 read 120000 us n30 3072 KB write 0 us n0 0 KB"
          " dir 0 us n0 decode 0 us commit 0 us");
}

namespace {
uint32_t gHookWindow = 0, gHookCalls = 0;
void hook(uint32_t w) { gHookWindow = w; gHookCalls++; }
}  // namespace

TEST(report_hook_same_window) {
    using namespace ot::diag;
    Telemetry::setReportHook(&hook);
    Telemetry t;
    CHECK(!t.due(1000));
    CHECK(!t.due(5999));
    CHECK(gHookCalls == 0);
    CHECK(t.due(6200));
    CHECK(gHookCalls == 1 && gHookWindow == 5200);
    CHECK(!t.due(6300));
    CHECK(gHookCalls == 1);
    Telemetry::setReportHook(nullptr);
    CHECK(t.due(12000));
    CHECK(gHookCalls == 1);
}

