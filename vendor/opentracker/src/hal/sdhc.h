// OpenTracker -- SD card on the K66 SDHC, 4-bit bus (spec 01 §6). ADMA2 multi-block
// transfers with CMD6 high speed (like stock); polled PIO kept as runtime fallback.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {
namespace sdhc {

struct Info {
    bool adma = false;         // ADMA2 in use (false: polled PIO through DATPORT)
    bool highSpeed = false;    // CMD6 switch to high-speed timing accepted by the card
    uint32_t clockHz = 0;      // current transfer SDCLK
    uint32_t retries = 0;      // transfers retried after an error
    uint32_t degrades = 0;     // clock/mode fallbacks after repeated errors
    uint32_t bounced = 0;      // chunks copied through the SRAM bounce buffer
    uint32_t admaErr = 0;      // ADMAES of the last ADMA error
};

// Cumulative transfer counters (telemetry: deltas per window).
struct Counters {
    uint32_t cmds = 0;         // commands issued (incl. data commands, CMD12/13 retries)
    uint32_t readBytes = 0, writeBytes = 0;
    uint32_t direct = 0;       // ADMA2 chunks straight to/from the caller's buffer
    uint32_t bounced = 0;      // chunks copied through the SRAM bounce buffer
};

enum class DirectCheck : uint8_t { NotRun = 0, Ok, Mismatch, XferError, Skipped };
const char* directCheckName(DirectCheck c);

bool cardPresent();                 // spec 01 §6: PTD0 LOW = inserted
bool init();                        // identify + 4-bit + transfer clock; true when ready
bool ready();
void release();                     // stop using the card (unmount / removal / storage mode)
uint32_t sectorCount();             // 512-byte sectors, 0 if unknown
// The app's path (FatFs disk I/O, selftest): refused while the computer owns
// the card (USB Storage mode).
bool readBlocks(uint32_t lba, uint8_t* dst, uint32_t count);
bool writeBlocks(uint32_t lba, const uint8_t* src, uint32_t count);

// 2.x USB Storage mode (src/hal/usb_storage.cpp): who may move data. Exactly
// one owner at a time: readBlocks / writeBlocks work only for kApp,
// hostRead / hostWrite only for kHost; the other path is refused and counted
// (ownershipViolations(), logged). init() / release() are not gated.
enum class Owner : uint8_t { kApp = 0, kHost };
void setOwner(Owner o);
Owner owner();
uint32_t ownershipViolations();
bool hostReadBlocks(uint32_t lba, uint8_t* dst, uint32_t count);
bool hostWriteBlocks(uint32_t lba, const uint8_t* src, uint32_t count);
// The SRAM bounce buffer (kSdBounceBytes, uncached SRAM_U, 32-byte aligned)
// for the USB data stage while the computer owns the card: nothing else uses
// it then (no FatFs transfer can bounce). Null unless the owner is kHost.
uint8_t* hostBuffer(uint32_t* bytes);
uint32_t lastError();               // IRQSTAT bits of the last failure (diagnostics)
const Info& info();
const char* initNote();             // why ADMA/high speed were not used ("" when both are)
const Counters& counters();

// ADMA2 straight into/out of SDRAM (no bounce). Reads: gated by
// checkSdramDirect(), called once after the SDRAM and its cache are up, with
// >= 65 KB + 64 B of free SDRAM scratch and a readable LBA with real data (the
// FAT); it only reads the card. Writes: bounce until a direct-write trial
// passed (SdStorage runs it lazily, once per boot, on a FatFs temp file:
// beginDirectWriteTrial() ... endDirectWriteTrial(ok)). Returns true when
// direct reads are on.
bool checkSdramDirect(uint8_t* scratch, uint32_t lba);
bool sdramDirectRead();
bool sdramDirectWrite();
DirectCheck directCheck();          // read check result
DirectCheck directWriteCheck();     // write check result (NotRun until the first SDRAM write)
void beginDirectWriteTrial();       // SDRAM writes go direct (if direct reads are on)
void endDirectWriteTrial(bool ok);  // keep them direct only if ok
void skipDirectWriteCheck();        // no trial possible: writes keep bouncing

// ---- pure helpers (host-tested in tests/hw_math_test.cpp) --------------------
namespace detail {
// K66 RM SDHC "ADMA2 descriptor": attr bits Valid(0) End(1) Int(2) Act(5:4),
// Act = 10b "transfer data"; length in bits 31:16 (16 bits); then 32-bit address.
constexpr uint32_t kAdmaValid = 1u << 0, kAdmaEnd = 1u << 1, kAdmaTran = 2u << 4;
constexpr uint32_t adma2Attr(uint32_t len, bool last) {
    return (len << 16) | kAdmaValid | kAdmaTran | (last ? kAdmaEnd : 0u);
}
// Number of descriptors for `bytes` with at most `perDesc` bytes each.
constexpr uint32_t adma2DescCount(uint32_t bytes, uint32_t perDesc) {
    return (bytes + perDesc - 1u) / perDesc;
}

// K66 memory map: SRAM_L 0x1FFF0000..0x1FFFFFFF, SRAM_U 0x20000000..0x2002FFFF.
constexpr uint32_t kSramLBase = 0x1FFF0000u, kSramUBase = 0x20000000u, kSramUEnd = 0x20030000u;
// ADMA2 may target `addr..addr+len` directly: word aligned (K66 ADMA2 address
// alignment), inside one SRAM half (a burst must not cross the SRAM_L/SRAM_U
// boundary), or inside SDRAM when that path is allowed.
constexpr bool dmaDirectOk(uint32_t addr, uint32_t len, bool allowSdram, uint32_t sdramBase,
                           uint32_t sdramSize) {
    return (addr & 3u) == 0 && len > 0 &&
           ((addr >= kSramLBase && addr + len <= kSramUBase) ||
            (addr >= kSramUBase && addr + len <= kSramUEnd) ||
            (allowSdram && addr >= sdramBase && addr - sdramBase + len <= sdramSize));
}
// One ADMA2 chunk of a transfer: direct into the caller's buffer when
// dmaDirectOk() allows it (at most maxDirect blocks), else through the bounce
// buffer (at most bounceBlocks). ADMA2 needs word-aligned addresses and every
// sector of a 2-byte-aligned buffer stays 2-byte aligned, so such buffers (e.g.
// int16 sample-bank destinations) bounce entirely: no head/tail split helps.
struct Chunk { bool direct; uint32_t blocks; };
constexpr Chunk planChunk(uint32_t addr, uint32_t count, uint32_t maxDirect, uint32_t bounceBlocks,
                          bool allowSdram, uint32_t sdramBase, uint32_t sdramSize) {
    return dmaDirectOk(addr, (count < maxDirect ? count : maxDirect) * 512u, allowSdram, sdramBase, sdramSize)
               ? Chunk{true, count < maxDirect ? count : maxDirect}
               : Chunk{false, count < bounceBlocks ? count : bounceBlocks};
}
// Core accesses to SRAM_L go through the LMEM code-bus cache; DMA does not.
constexpr bool inCodeBusRam(uint32_t addr) { return addr >= kSramLBase && addr < kSramUBase; }

// SD Physical Layer spec §4.3.10, CMD6 64-byte status (big-endian bit 511 first):
// bits 415:400 = group 1 support (bit 401 = high speed), bits 379:376 = group 1
// function selected (0xF = cannot switch).
inline bool cmd6HighSpeedSupported(const uint8_t* st) { return (st[13] & 0x02u) != 0; }
inline uint8_t cmd6Group1Result(const uint8_t* st) { return st[16] & 0x0Fu; }
}  // namespace detail

// The SD bounce buffer (kSdBounceBytes, uncached SRAM_U) as scratch for the
// fault handler (diag.cpp): stops the SDHC data line first, so no DMA writes
// into it any more. Only for code that never returns to SD I/O.
uint8_t* takeBounceForFault(uint32_t* bytes);

}  // namespace sdhc
}  // namespace hal
}  // namespace ot
