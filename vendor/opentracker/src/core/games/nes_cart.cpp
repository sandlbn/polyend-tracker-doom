// OpenTracker NES core: iNES loading and the cartridge mappers 0, 1, 2, 3, 4
// and 7 (register behaviour per the public NESdev mapper documentation).
// The bundled homebrew titles need 0 (NROM) and 4 (MMC3) (PLAN B9).
#include <cstring>

#include "games/nes.h"

namespace ot {
namespace games {

const char* nesErrorText(NesError e) {
    switch (e) {
        case NesError::Ok: return "OK";
        case NesError::BadHeader: return "Not an iNES ROM";
        case NesError::Truncated: return "ROM file is truncated";
        case NesError::BankSize: return "Invalid ROM bank size";
        case NesError::Mapper: return "Mapper not supported";
    }
    return "ROM error";
}

NesError Nes::load(const uint8_t* img, uint32_t size, const NesMemory& mem) {
    loaded_ = false;
    jam_ = false;
    if (!img || size < 16 || std::memcmp(img, "NES\x1A", 4) != 0) return NesError::BadHeader;
    if (!mem.chrRam || !mem.prgRam || !mem.vramExtra || !mem.audio || !mem.spriteLines) return NesError::BadHeader;
    const uint8_t f6 = img[6], f7 = img[7];
    // NES 2.0 ((f7 & 0x0C) == 8) keeps byte 7; an archaic header with junk in
    // bytes 7..15 (e.g. "DiskDude!") has its upper mapper nibble ignored
    // (spec 14 §5.1).
    bool dirty = false;
    if ((f7 & 0x0C) != 0x08)
        for (int i = 12; i < 16; ++i) dirty = dirty || img[i] != 0;
    mapper_ = uint8_t((f6 >> 4) | (dirty ? 0 : (f7 & 0xF0)));
    const uint32_t prg16 = img[4], chr8 = img[5];
    if (prg16 == 0) return NesError::BankSize;
    const uint32_t prgOff = 16 + ((f6 & 4) ? 512u : 0u);
    const uint32_t prgBytes = prg16 * 16384u, chrBytes = chr8 * 8192u;
    if (uint64_t(prgOff) + prgBytes + chrBytes > size) return NesError::Truncated;
    switch (mapper_) {
        case 0: case 1: case 2: case 3: case 4: case 7: break;
        default: return NesError::Mapper;
    }
    mem_ = mem;
    prgRom_ = img + prgOff;
    prgBanks8_ = prgBytes / 8192u;
    chrIsRam_ = chr8 == 0;
    // CHR ROM is never written (vramWrite checks chrIsRam_).
    chrBase_ = chrIsRam_ ? mem.chrRam : const_cast<uint8_t*>(img + prgOff + prgBytes);
    chrBanks1_ = chrIsRam_ ? kChrRamBytes / 1024u : chrBytes / 1024u;
    fourScreen_ = f6 & 8;
    hdrMirror_ = (f6 & 1) ? kVertical : kHorizontal;
    loaded_ = true;
    reset();
    return NesError::Ok;
}

void Nes::reset() {
    if (!loaded_) return;
    // Power-on state.
    std::memset(ram_, 0, sizeof(ram_));
    std::memset(vram_, 0, sizeof(vram_));
    std::memset(oam_, 0xFF, sizeof(oam_));
    std::memset(pal_, 0x0F, sizeof(pal_));
    std::memset(mem_.prgRam, 0, kPrgRamBytes);
    std::memset(mem_.vramExtra, 0, kVramExtraBytes);
    if (chrIsRam_) std::memset(mem_.chrRam, 0, kChrRamBytes);
    a_ = x_ = y_ = 0;
    s_ = 0xFD;
    fc_ = fz_ = fd_ = fv_ = fn_ = false;
    fi_ = true;
    nmi_ = jam_ = false;
    irq_ = 0;
    dmaStall_ = 0;
    dot_ = 0;
    cycles_ = 0;
    frames_ = 0;
    ctrl_ = mask_ = status_ = oamAddr_ = readBuf_ = bus_ = 0;
    v_ = t_ = 0;
    fineX_ = 0;
    w_ = odd_ = a12_ = false;
    oamDirty_ = colorDirty_ = true;
    markAllDirty();
    pad_ = padShift_ = padStrobe_ = 0;
    std::memset(mreg_, 0, sizeof(mreg_));
    mmc1Shift_ = 0x10;
    mmc3Counter_ = mmc3Latch_ = 0;
    mmc3Reload_ = mmc3IrqOn_ = false;
    // APU
    p1_ = Pulse{};
    p1_.ones = true;
    p2_ = Pulse{};
    tri_ = Triangle{};
    noise_ = Noise{};
    dmc_ = Dmc{};
    apuCycles_ = 0;
    fcPos_ = 0;
    fcFive_ = fcInhibit_ = false;
    fcSchedule();
    if (!samplePeriod16_) setSampleRate(44100);
    sampleLeft16_ = samplePeriod16_;
    acc_ = 0;
    accCycles_ = 0;
    hpPrevIn_ = hpPrevOut_ = 0.0f;
    audioCount_ = 0;
    // Cartridge
    setMirror(fourScreen_ ? kFour : hdrMirror_);
    setChr8(0);
    switch (mapper_) {
        case 1:
            mreg_[0] = 0x0C;   // control: PRG mode 3 (last bank fixed)
            mmc1Apply();
            break;
        case 4:
            mmc3Apply();
            break;
        case 7:
            setPrg32(0);
            setMirror(kSingle0);
            break;
        default:   // 0, 2, 3: first 16 KB at $8000, last at $C000
            setPrg16(0, 0);
            setPrg16(1, -1);
            break;
    }
    pc_ = uint16_t(peek(0xFFFC) | (peek(0xFFFD) << 8));
}

// ---------------------------------------------------------------------------
// Bank switching (bank numbers wrap at the ROM size; negative = from the end)

void Nes::setPrg8(int slot, int bank) {
    const int n = int(prgBanks8_);
    bank %= n;
    if (bank < 0) bank += n;
    prg_[slot & 3] = prgRom_ + uint32_t(bank) * 8192u;
}

void Nes::setPrg16(int slot, int bank) {
    const int n = int(prgBanks8_ / 2 ? prgBanks8_ / 2 : 1);
    bank %= n;
    if (bank < 0) bank += n;
    setPrg8(slot * 2, bank * 2);
    setPrg8(slot * 2 + 1, prgBanks8_ >= 2 ? bank * 2 + 1 : bank * 2);
}

void Nes::setPrg32(int bank) {
    setPrg16(0, bank * 2);
    setPrg16(1, bank * 2 + 1);
}

void Nes::setChr1(int slot, int bank) {
    const int n = int(chrBanks1_);
    bank %= n;
    if (bank < 0) bank += n;
    chr_[slot & 7] = chrBase_ + uint32_t(bank) * 1024u;
}

void Nes::setChr4(int slot, int bank) {
    for (int i = 0; i < 4; ++i) setChr1(slot * 4 + i, bank * 4 + i);
}

void Nes::setChr8(int bank) {
    for (int i = 0; i < 8; ++i) setChr1(i, bank * 8 + i);
}

void Nes::setMirror(Mirror m) {
    uint8_t* a = vram_;
    uint8_t* b = vram_ + 1024;
    switch (m) {
        case kHorizontal: nt_[0] = a; nt_[1] = a; nt_[2] = b; nt_[3] = b; break;
        case kVertical: nt_[0] = a; nt_[1] = b; nt_[2] = a; nt_[3] = b; break;
        case kSingle0: nt_[0] = nt_[1] = nt_[2] = nt_[3] = a; break;
        case kSingle1: nt_[0] = nt_[1] = nt_[2] = nt_[3] = b; break;
        case kFour:
            nt_[0] = a;
            nt_[1] = b;
            nt_[2] = mem_.vramExtra;
            nt_[3] = mem_.vramExtra + 1024;
            break;
    }
}

// ---------------------------------------------------------------------------
// Mapper registers

// MMC1: mreg_[0] control, [1] CHR 0, [2] CHR 1, [3] PRG.
void Nes::mmc1Apply() {
    const uint8_t ctl = mreg_[0];
    if (!fourScreen_) {
        static const Mirror kM[4] = {kSingle0, kSingle1, kVertical, kHorizontal};
        setMirror(kM[ctl & 3]);
    }
    if (ctl & 0x10) {
        setChr4(0, mreg_[1]);
        setChr4(1, mreg_[2]);
    } else {
        setChr4(0, mreg_[1] & 0x1E);
        setChr4(1, (mreg_[1] & 0x1E) | 1);
    }
    // SUROM (512 KB PRG): CHR register bit 4 selects the 256 KB half.
    const int outer = prgBanks8_ > 32 ? (mreg_[1] & 0x10) : 0;
    const int p = mreg_[3] & 0x0F;
    switch ((ctl >> 2) & 3) {
        case 0:
        case 1:
            setPrg16(0, outer + (p & 0x0E));
            setPrg16(1, outer + (p | 1));
            break;
        case 2:
            setPrg16(0, outer);
            setPrg16(1, outer + p);
            break;
        default:
            setPrg16(0, outer + p);
            setPrg16(1, outer + 15 < int(prgBanks8_ / 2) ? outer + 15 : -1);
            break;
    }
}

// MMC3: mreg_[0..7] R0..R7, [8] bank select.
void Nes::mmc3Apply() {
    const uint8_t sel = mreg_[8];
    if (sel & 0x40) {
        setPrg8(0, -2);
        setPrg8(2, mreg_[6]);
    } else {
        setPrg8(0, mreg_[6]);
        setPrg8(2, -2);
    }
    setPrg8(1, mreg_[7]);
    setPrg8(3, -1);
    const int inv = (sel & 0x80) ? 4 : 0;   // CHR A12 inversion
    setChr1(inv + 0, mreg_[0] & 0xFE);
    setChr1(inv + 1, mreg_[0] | 1);
    setChr1(inv + 2, mreg_[1] & 0xFE);
    setChr1(inv + 3, mreg_[1] | 1);
    setChr1((inv ^ 4) + 0, mreg_[2]);
    setChr1((inv ^ 4) + 1, mreg_[3]);
    setChr1((inv ^ 4) + 2, mreg_[4]);
    setChr1((inv ^ 4) + 3, mreg_[5]);
}

void Nes::mapperWrite(uint16_t a, uint8_t v) {
    switch (mapper_) {
        case 1:
            if (v & 0x80) {
                mmc1Shift_ = 0x10;
                mreg_[0] |= 0x0C;
                mmc1Apply();
                return;
            } else {
                const bool full = mmc1Shift_ & 1;
                mmc1Shift_ = uint8_t((mmc1Shift_ >> 1) | ((v & 1) << 4));
                if (full) {
                    mreg_[(a >> 13) & 3] = mmc1Shift_ & 0x1F;
                    mmc1Shift_ = 0x10;
                    mmc1Apply();
                }
            }
            return;
        case 2:
            setPrg16(0, v);
            return;
        case 3:
            setChr8(v);
            return;
        case 4:
            switch (a & 0xE001) {
                case 0x8000: mreg_[8] = v; mmc3Apply(); break;
                case 0x8001: mreg_[mreg_[8] & 7] = v; mmc3Apply(); break;
                case 0xA000:
                    if (!fourScreen_) setMirror((v & 1) ? kHorizontal : kVertical);
                    break;
                case 0xC000: mmc3Latch_ = v; break;
                case 0xC001: mmc3Counter_ = 0; mmc3Reload_ = true; break;
                case 0xE000: mmc3IrqOn_ = false; irq_ &= uint8_t(~4); break;
                case 0xE001: mmc3IrqOn_ = true; break;
                default: break;   // $A001 PRG RAM protect: RAM always enabled
            }
            return;
        case 7:
            setPrg32(v & 0x0F);
            setMirror((v & 0x10) ? kSingle1 : kSingle0);
            return;
        default:
            return;
    }
}

void Nes::mapperLine() {
    if (mapper_ != 4) return;
    // MMC3 scanline counter (rising A12 once per rendered line).
    if (mmc3Counter_ == 0 || mmc3Reload_) {
        mmc3Counter_ = mmc3Latch_;
        mmc3Reload_ = false;
    } else {
        --mmc3Counter_;
    }
    if (mmc3Counter_ == 0 && mmc3IrqOn_) irq_ |= 4;
}

}  // namespace games
}  // namespace ot
