// OpenTracker NES core (spec 14 §6 interface contract).
//
// A compact, scanline-based NES (RP2A03 + RP2C02, NTSC) emulator written for
// OpenTracker from public hardware documentation (6502 instruction set, the
// NES PPU/APU register model as documented by the NESdev community). It
// contains no third-party emulator code (spec 14 §5.4: the stock core is GPL
// and must not be used).
//
// Design for a 180 MHz Cortex-M4 (spec 14 §3.3):
//  - CPU: switch-dispatched 6502 incl. the stable unofficial opcodes,
//    instruction-granular timing (no dummy bus cycles).
//  - PPU: whole scanlines are rendered at dot 0 from the scroll registers;
//    CPU time inside a line is split at the sprite-0 hit dot, dot 257
//    (scroll increment/copy) and dot 260 (MMC3 A12 clock). Raster effects
//    are therefore line-accurate, not dot-accurate.
//  - Frame skip: runFrame(nullptr) emulates without drawing; sprite-0 hits
//    are still computed for the lines that need them.
//  - APU: two pulses, triangle, noise and DMC, synced per scanline, box
//    filtered to the output rate (mono, spec 14 §4).
//  - Mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), 7 (AxROM).
//
// Memory: the object itself holds only the hot state (2 KB CPU RAM, 2 KB
// nametable RAM, OAM, palette, line buffers: ~5 KB, meant for internal
// SRAM). The ROM image, CHR RAM, PRG RAM and four-screen VRAM are caller
// buffers (SDRAM on the device).
#pragma once

#include <cstdint>

namespace ot {
namespace games {

enum class NesError : uint8_t {
    Ok = 0,
    BadHeader,      // not an iNES image
    Truncated,      // header promises more PRG/CHR than the file holds
    BankSize,       // no PRG ROM
    Mapper,         // mapper number not supported (mapper())
};
const char* nesErrorText(NesError e);

// Buffers the caller provides (sizes below), valid while the game is loaded.
struct NesMemory {
    uint8_t* chrRam = nullptr;      // kChrRamBytes (used when the cart has no CHR ROM)
    uint8_t* prgRam = nullptr;      // kPrgRamBytes ($6000-$7FFF)
    uint8_t* vramExtra = nullptr;   // kVramExtraBytes (four-screen carts)
    int16_t* audio = nullptr;       // kAudioCap mono samples written per frame
    uint64_t* spriteLines = nullptr;   // Nes::kHeight entries (sprite cover per line)
};

class Nes {
public:
    static constexpr int kWidth = 256, kHeight = 240;
    static constexpr uint32_t kCpuHz = 1789773;     // NTSC 21.477272 MHz / 12
    static constexpr uint32_t kFrameUs = 16639;     // 89341.5 dots / 5.369318 MHz
    static constexpr uint32_t kChrRamBytes = 8192;
    static constexpr uint32_t kPrgRamBytes = 8192;
    static constexpr uint32_t kVramExtraBytes = 2048;
    static constexpr uint32_t kAudioCap = 1024;     // > 44100 / 60 samples per frame

    // Pad bits (spec 14 §6 item 3; NES serial order).
    enum Pad : uint8_t { kA = 1, kB = 2, kSelect = 4, kStart = 8, kUp = 16, kDown = 32, kLeft = 64, kRight = 128 };

    // Parse an iNES image (kept in place, must stay valid) and power on.
    NesError load(const uint8_t* image, uint32_t size, const NesMemory& mem);
    bool loaded() const { return loaded_; }
    void unload() { loaded_ = false; }
    void reset();                         // power-on state
    int mapper() const { return mapper_; }

    // Output colour per NES colour index (0..63), e.g. RGB332 bytes.
    void setPalette(const uint8_t lut[64]);
    void setSampleRate(uint32_t hz);

    // Emulate one NTSC frame (262 lines). fb = 256 x 240 output bytes, or
    // nullptr to skip drawing (frame skip). fb keeps the previous frame:
    // only rows that changed are rewritten and marked dirty (fb must be
    // 4-byte aligned). False when the
    // CPU has halted on a JAM opcode (the game is broken; jammed() stays true).
    bool runFrame(uint8_t* fb);
    bool rowDirty(int y) const { return (dirty_[y >> 5] >> (y & 31)) & 1; }
    void clearDirty();
    void markAllDirty();
    bool jammed() const { return jam_; }

    void setPad(uint8_t bits) { pad_ = bits; }
    // Mono samples produced by the last runFrame (in NesMemory::audio).
    uint32_t audioCount() const { return audioCount_; }

    // Debug / tests: CPU bus read without side effects (RAM, cart space).
    uint8_t peek(uint16_t addr) const;
    uint16_t pc() const { return pc_; }
    uint32_t frameCount() const { return frames_; }
    uint64_t cpuCycles() const { return cycles_; }

    // NES colour (0..63) -> 0xRRGGBB, generated from an NTSC signal model.
    static uint32_t nesColor(uint8_t index);
    static void rgb332Lut(uint8_t out[64]);

private:
    // ---- CPU (nes_cpu.cpp)
    void runTo(int32_t dot);              // execute until the CPU reaches PPU dot `dot`
    inline uint8_t rdBus(uint16_t a, uint64_t now);
    uint8_t rdSlow(uint16_t a);           // $2000-$7FFF
    void wrSlow(uint16_t a, uint8_t v);   // $2000-$FFFF

    // ---- PPU (nes_ppu.cpp)
    uint8_t ppuRead(uint16_t reg);
    void ppuWrite(uint16_t reg, uint8_t v);
    uint8_t vramRead(uint16_t a) const;
    void vramWrite(uint16_t a, uint8_t v);
    bool renderingOn() const { return (mask_ & 0x18) != 0; }
    int renderLine(int line);             // sprite-0 hit x, or -1
    void blankLine(int line);
    void refreshColors();
    void commitLine(int line);
    void buildSpriteLines(int spH);
    void incY();
    void a12(uint16_t addr);
    void copyX() { v_ = uint16_t((v_ & ~0x041F) | (t_ & 0x041F)); }
    void copyY() { v_ = uint16_t((v_ & ~0x7BE0) | (t_ & 0x7BE0)); }

    // ---- APU (nes_apu.cpp)
    struct Envelope {
        bool start = false, loop = false, constant = false;
        uint8_t period = 0, divider = 0, decay = 0;
        void clock();
        uint8_t volume() const { return constant ? period : decay; }
    };
    struct Pulse {
        Envelope env;
        uint8_t duty = 0, seq = 0, length = 0;
        bool enabled = false, halt = false;
        bool sweepOn = false, sweepNeg = false, sweepReload = false;
        uint8_t sweepPeriod = 0, sweepShift = 0, sweepDiv = 0;
        uint16_t timer = 0;
        int32_t timerLeft = 2;
        bool ones = false;                // pulse 1 negate uses ones' complement
        bool muted() const;
        uint16_t target() const;
        int out() const;
        int32_t integrate(int32_t n);
    };
    struct Triangle {
        uint8_t seq = 0, length = 0, linear = 0, linearReload = 0;
        bool enabled = false, control = false, reloadFlag = false;
        uint16_t timer = 0;
        int32_t timerLeft = 1;
        int32_t integrate(int32_t n);
    };
    struct Noise {
        Envelope env;
        uint8_t length = 0;
        bool enabled = false, halt = false, mode = false;
        uint16_t period = 4, lfsr = 1;
        int32_t timerLeft = 4;
        int out() const { return (lfsr & 1) || !length ? 0 : env.volume(); }
        int32_t integrate(int32_t n);
    };
    struct Dmc {
        bool irqOn = false, loop = false, silence = true, bufFull = false;
        uint8_t level = 0, shift = 0, bits = 8, buf = 0;
        uint16_t rate = 428, startAddr = 0xC000, startLen = 1, addr = 0xC000, left = 0;
        int32_t timerLeft = 428;
    };
    void apuWrite(uint16_t a, uint8_t v);
    uint8_t apuStatus();
    void apuSync();
    void apuRun(int32_t n);
    void fcSchedule();
    void apuQuarter();
    void apuHalf();
    int32_t dmcIntegrate(int32_t n);
    void dmcFetch();
    void emitSample(float v);

    // ---- cartridge (nes_cart.cpp)
    void mapperWrite(uint16_t a, uint8_t v);
    void mapperLine();                    // MMC3 A12 clock, once per rendered line
    void setPrg8(int slot, int bank);     // slot 0..3 = $8000..$E000
    void setPrg16(int slot, int bank);
    void setPrg32(int bank);
    void setChr1(int slot, int bank);     // slot 0..7 = $0000..$1C00
    void setChr4(int slot, int bank);
    void setChr8(int bank);
    enum Mirror : uint8_t { kHorizontal, kVertical, kSingle0, kSingle1, kFour };
    void setMirror(Mirror m);
    void mmc1Apply();
    void mmc3Apply();

    // CPU registers.
    uint16_t pc_ = 0;
    uint8_t a_ = 0, x_ = 0, y_ = 0, s_ = 0xFD;
    bool fc_ = false, fz_ = false, fi_ = true, fd_ = false, fv_ = false, fn_ = false;
    bool nmi_ = false, jam_ = false;
    uint8_t irq_ = 0;                     // level sources: 1 APU frame, 2 DMC, 4 mapper
    int32_t dmaStall_ = 0;                // OAM DMA cycles added to the current instruction
    int32_t dot_ = 0;                     // PPU dot of the CPU inside the current frame
    uint64_t cycles_ = 0;
    uint32_t frames_ = 0;

    // Memory map.
    uint8_t ram_[2048];
    const uint8_t* prg_[4] = {};          // 8 KB windows $8000..$FFFF
    uint8_t* chr_[8] = {};                // 1 KB windows $0000..$1FFF
    uint8_t* nt_[4] = {};                 // nametables $2000/$2400/$2800/$2C00
    uint8_t vram_[2048];
    const uint8_t* prgRom_ = nullptr;
    uint8_t* chrBase_ = nullptr;          // CHR ROM (cast) or CHR RAM
    uint32_t prgBanks8_ = 0, chrBanks1_ = 0;
    bool chrIsRam_ = false;
    NesMemory mem_;
    uint8_t mapper_ = 0;
    bool fourScreen_ = false, loaded_ = false;
    Mirror hdrMirror_ = kHorizontal;
    uint8_t pad_ = 0, padShift_ = 0, padStrobe_ = 0;
    uint8_t mreg_[16] = {};               // mapper registers
    uint8_t mmc1Shift_ = 0x10;
    uint8_t mmc3Counter_ = 0, mmc3Latch_ = 0;
    bool mmc3Reload_ = false, mmc3IrqOn_ = false;

    // PPU state.
    uint8_t ctrl_ = 0, mask_ = 0, status_ = 0, oamAddr_ = 0, readBuf_ = 0, bus_ = 0;
    uint16_t v_ = 0, t_ = 0;
    uint8_t fineX_ = 0;
    bool w_ = false, odd_ = false, a12_ = false;
    uint8_t oam_[256];
    uint8_t pal_[32];
    uint8_t lut_[64];
    uint8_t* fb_ = nullptr;
    uint8_t bgLine_[8 + 33 * 8];          // 8 px of fine-X slack before pixel 0
    uint8_t spLine_[256];
    uint8_t color_[32];                   // output byte per palette index (lut_ o pal_)
    bool colorDirty_ = true;
    alignas(4) uint8_t outLine_[256];     // composed line (internal RAM), then compared into fb_
    uint32_t dirty_[8] = {};              // rows changed since clearDirty()
    bool oamDirty_ = true;
    uint8_t spHBuilt_ = 0;

    // APU state.
    Pulse p1_, p2_;
    Triangle tri_;
    Noise noise_;
    Dmc dmc_;
    uint64_t apuCycles_ = 0;              // CPU cycle the APU has reached
    int32_t fcPos_ = 0;                   // frame counter position (CPU cycles)
    int32_t fcNext_ = 7457;               // next frame-sequencer event
    bool fcFive_ = false, fcInhibit_ = false;
    int32_t sampleLeft16_ = 0, samplePeriod16_ = 0;   // 16.16 CPU cycles to the next sample
    int32_t acc_ = 0;                     // integrated mix in the current sample (1e-5 units)
    int32_t accCycles_ = 0;
    float hpPrevIn_ = 0.0f, hpPrevOut_ = 0.0f;
    uint32_t audioCount_ = 0;
};

}  // namespace games
}  // namespace ot
