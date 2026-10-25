// OpenTracker NES core: 6502 CPU (RP2A03, no decimal mode) and the CPU bus.
// Instruction set and cycle counts per the public MOS 6502 documentation;
// unofficial opcodes per the NESdev "CPU unofficial opcodes" description.
//
// Performance (spec 14 §3.3, 180 MHz Cortex-M4): runTo() keeps the 6502
// registers in locals for the whole run and dispatches with one switch;
// zero-page and stack accesses go straight to internal RAM, $8000-$FFFF reads
// through the four 8 KB bank pointers. Only I/O ($2000-$5FFF) leaves the loop,
// with the current CPU cycle published in cycles_ (the APU syncs to it).
#include "games/nes.h"

// Everything in the interpreter loop must inline: an outlined helper would
// force the register locals it captures into memory.
#define OT_ALWAYS_INLINE __attribute__((always_inline))

namespace ot {
namespace games {

namespace {
// Base cycles per opcode (page-cross and branch penalties are added at run time).
constexpr uint8_t kCycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,   // 00
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 10
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,   // 20
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 30
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,   // 40
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 50
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,   // 60
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 70
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,   // 80
    2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,   // 90
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,   // A0
    2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,   // B0
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,   // C0
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // D0
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,   // E0
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // F0
};
}  // namespace

// ---------------------------------------------------------------------------
// CPU bus

OT_ALWAYS_INLINE inline uint8_t Nes::rdBus(uint16_t a, uint64_t now) {
    if (a >= 0x8000) return prg_[(a >> 13) & 3][a & 0x1FFF];
    if (a < 0x2000) return ram_[a & 0x7FF];
    cycles_ = now;
    return rdSlow(a);
}

uint8_t Nes::rdSlow(uint16_t a) {
    if (a >= 0x6000) return mem_.prgRam[a & 0x1FFF];
    if (a < 0x4000) return ppuRead(a & 7);
    switch (a) {
        case 0x4015: return apuStatus();
        case 0x4016: {
            // Standard controller: 8 bits A..Right, then 1s (open-bus bit 6 set).
            uint8_t bit;
            if (padStrobe_ & 1) {
                bit = pad_ & 1;
            } else {
                bit = padShift_ & 1;
                padShift_ = uint8_t(0x80 | (padShift_ >> 1));
            }
            return uint8_t(0x40 | bit);
        }
        case 0x4017: return 0x40;   // pad 2 never pressed (spec 14 §2.2)
        default: return uint8_t(a >> 8);   // open bus
    }
}

void Nes::wrSlow(uint16_t a, uint8_t v) {
    if (a >= 0x8000) {
        mapperWrite(a, v);
    } else if (a >= 0x6000) {
        mem_.prgRam[a & 0x1FFF] = v;
    } else if (a < 0x4000) {
        ppuWrite(a & 7, v);
    } else if (a == 0x4014) {
        // OAM DMA: 256 bytes from page v; the CPU stalls 513 (+1 on odd) cycles.
        const uint16_t base = uint16_t(v) << 8;
        for (int i = 0; i < 256; ++i) oam_[uint8_t(oamAddr_ + i)] = rdBus(uint16_t(base + i), cycles_);
        oamDirty_ = true;
        dmaStall_ += 513 + int32_t(cycles_ & 1);
    } else if (a == 0x4016) {
        padStrobe_ = v;
        padShift_ = pad_;
    } else if (a < 0x4018) {
        apuWrite(a, v);
    }
}

uint8_t Nes::peek(uint16_t a) const {
    if (!loaded_) return 0;
    if (a >= 0x8000) return prg_[(a >> 13) & 3][a & 0x1FFF];
    if (a < 0x2000) return ram_[a & 0x7FF];
    if (a >= 0x6000) return mem_.prgRam[a & 0x1FFF];
    return 0;
}

// ---------------------------------------------------------------------------
// Interpreter

void Nes::runTo(int32_t target) {
    if (jam_) {
        if (dot_ < target) dot_ = target;
        return;
    }
    // Registers and flags live in locals. N and Z are kept lazily: Z is set
    // when zr == 0, N is bit 7 of nr (BIT sets them independently).
    uint16_t pc = pc_;
    uint8_t a = a_, x = x_, y = y_, s = s_;
    bool fc = fc_, fv = fv_, fi = fi_, fd = fd_;
    uint8_t zr = fz_ ? 0 : 1, nr = fn_ ? 0x80 : 0;
    int32_t dot = dot_;
    uint64_t cyc = cycles_;

    // Bus access; I/O first publishes the current CPU cycle (APU sync, DMA parity).
#define OT_RD(A) rdBus(A, cyc)
#define OT_WR(A, V)                                  \
    do {                                             \
        const uint16_t wa_ = (A);                    \
        const uint8_t wv_ = (V);                     \
        if (wa_ < 0x2000) {                          \
            ram_[wa_ & 0x7FF] = wv_;                 \
        } else {                                     \
            cycles_ = cyc;                           \
            wrSlow(wa_, wv_);                        \
        }                                            \
    } while (0)
#define OT_ZN(R) (zr = nr = (R))
#define OT_P(B)                                                                                          \
    uint8_t((fc ? 0x01 : 0) | (zr == 0 ? 0x02 : 0) | (fi ? 0x04 : 0) | (fd ? 0x08 : 0) | ((B) ? 0x10 : 0) | \
            0x20 | (fv ? 0x40 : 0) | (nr & 0x80))
#define OT_SETP(P)                    \
    do {                              \
        const uint8_t p_ = (P);       \
        fc = p_ & 0x01;               \
        zr = (p_ & 0x02) ? 0 : 1;     \
        fi = p_ & 0x04;               \
        fd = p_ & 0x08;               \
        fv = p_ & 0x40;               \
        nr = p_ & 0x80;               \
    } while (0)
#define OT_PUSH(V) (ram_[0x100 | s--] = (V))
#define OT_POP() (ram_[0x100 | ++s])
    // Effective addresses. Zero-page pointers always come from internal RAM.
#define OT_IMM() (pc++)
#define OT_ZP() (OT_RD(pc++))
#define OT_ZPX() uint8_t(OT_RD(pc++) + x)
#define OT_ZPY() uint8_t(OT_RD(pc++) + y)
#define OT_ABS() (pc = uint16_t(pc + 2), uint16_t(OT_RD(uint16_t(pc - 2)) | (OT_RD(uint16_t(pc - 1)) << 8)))

    while (dot < target) {
        int n;
        if (nmi_ || (irq_ && !fi)) {
            uint16_t vec = 0xFFFE;
            if (nmi_) {
                vec = 0xFFFA;
                nmi_ = false;
            }
            OT_PUSH(uint8_t(pc >> 8));
            OT_PUSH(uint8_t(pc));
            OT_PUSH(OT_P(false));
            fi = true;
            pc = uint16_t(OT_RD(vec) | (OT_RD(uint16_t(vec + 1)) << 8));
            n = 7;
        } else {
            const uint8_t op = OT_RD(pc++);
            n = kCycles[op];
            uint16_t ea;
            uint8_t v;
            // Indexed absolute / (zp),Y with the read page-cross penalty.
            auto absIdx = [&](uint8_t idx, bool pen) OT_ALWAYS_INLINE -> uint16_t {
                const uint16_t base = OT_ABS();
                const uint16_t r = uint16_t(base + idx);
                if (pen && ((base ^ r) & 0x100)) ++n;
                return r;
            };
            auto indY = [&](bool pen) OT_ALWAYS_INLINE -> uint16_t {
                const uint8_t z = OT_RD(pc++);
                const uint16_t base = uint16_t(ram_[z] | (ram_[uint8_t(z + 1)] << 8));
                const uint16_t r = uint16_t(base + y);
                if (pen && ((base ^ r) & 0x100)) ++n;
                return r;
            };
            auto indX = [&]() OT_ALWAYS_INLINE -> uint16_t {
                const uint8_t z = uint8_t(OT_RD(pc++) + x);
                return uint16_t(ram_[z] | (ram_[uint8_t(z + 1)] << 8));
            };
            auto adc = [&](uint8_t m) OT_ALWAYS_INLINE {
                const unsigned sum = unsigned(a) + m + (fc ? 1u : 0u);
                fv = (~(a ^ m) & (a ^ sum) & 0x80) != 0;
                fc = sum > 0xFF;
                a = uint8_t(sum);
                OT_ZN(a);
            };
            auto cmp = [&](uint8_t r, uint8_t m) OT_ALWAYS_INLINE {
                fc = r >= m;
                OT_ZN(uint8_t(r - m));
            };
            auto asl = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                fc = m & 0x80;
                m = uint8_t(m << 1);
                OT_ZN(m);
                return m;
            };
            auto lsr = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                fc = m & 1;
                m = uint8_t(m >> 1);
                OT_ZN(m);
                return m;
            };
            auto rol = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                const bool c = fc;
                fc = m & 0x80;
                m = uint8_t((m << 1) | (c ? 1 : 0));
                OT_ZN(m);
                return m;
            };
            auto ror = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                const bool c = fc;
                fc = m & 1;
                m = uint8_t((m >> 1) | (c ? 0x80 : 0));
                OT_ZN(m);
                return m;
            };
            auto dec = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                m = uint8_t(m - 1);
                OT_ZN(m);
                return m;
            };
            auto inc = [&](uint8_t m) OT_ALWAYS_INLINE -> uint8_t {
                m = uint8_t(m + 1);
                OT_ZN(m);
                return m;
            };
            auto decq = [](uint8_t m) OT_ALWAYS_INLINE -> uint8_t { return uint8_t(m - 1); };   // DCP: flags from CMP
            auto incq = [](uint8_t m) OT_ALWAYS_INLINE -> uint8_t { return uint8_t(m + 1); };   // ISC: flags from SBC
            auto branch = [&](bool cond) OT_ALWAYS_INLINE {
                const int8_t off = int8_t(OT_RD(pc++));
                if (cond) {
                    const uint16_t to = uint16_t(pc + off);
                    n += ((to ^ pc) & 0x100) ? 2 : 1;
                    pc = to;
                }
            };
            // Unofficial "high byte + 1" stores (SHX/SHY/AHX/TAS): value & (H + 1);
            // on a page cross the value also replaces the address high byte.
            auto storeHi = [&](uint16_t base, uint8_t idx, uint8_t val) OT_ALWAYS_INLINE {
                uint16_t addr = uint16_t(base + idx);
                const uint8_t r = uint8_t(val & ((base >> 8) + 1));
                if ((base ^ addr) & 0x100) addr = uint16_t((r << 8) | (addr & 0xFF));
                OT_WR(addr, r);
            };

// Read-type group (ORA/AND/EOR/ADC/LDA/CMP/SBC): opcode bases xx01..xx1D.
#define OT_GROUP_READ(B, EXPR)                                   \
    case (B) + 0x01: v = OT_RD(indX()); EXPR; break;             \
    case (B) + 0x05: v = ram_[OT_ZP()]; EXPR; break;             \
    case (B) + 0x09: v = OT_RD(OT_IMM()); EXPR; break;           \
    case (B) + 0x0D: v = OT_RD(OT_ABS()); EXPR; break;           \
    case (B) + 0x11: v = OT_RD(indY(true)); EXPR; break;         \
    case (B) + 0x15: v = ram_[OT_ZPX()]; EXPR; break;            \
    case (B) + 0x19: v = OT_RD(absIdx(y, true)); EXPR; break;    \
    case (B) + 0x1D: v = OT_RD(absIdx(x, true)); EXPR; break;
// Read-modify-write group (ASL/ROL/LSR/ROR/DEC/INC): xx06, xx0E, xx16, xx1E.
#define OT_GROUP_RMW(B, FN)                                                  \
    case (B) + 0x06: ea = OT_ZP(); ram_[ea] = FN(ram_[ea]); break;           \
    case (B) + 0x0E: ea = OT_ABS(); v = FN(OT_RD(ea)); OT_WR(ea, v); break;  \
    case (B) + 0x16: ea = OT_ZPX(); ram_[ea] = FN(ram_[ea]); break;          \
    case (B) + 0x1E: ea = absIdx(x, false); v = FN(OT_RD(ea)); OT_WR(ea, v); break;
// Unofficial RMW + ALU group (SLO/RLA/SRE/RRA/DCP/ISC): xx03..xx1F.
#define OT_GROUP_COMBO(B, FN, EXPR)                                                     \
    case (B) + 0x03: ea = indX(); v = FN(OT_RD(ea)); OT_WR(ea, v); EXPR; break;         \
    case (B) + 0x07: ea = OT_ZP(); v = FN(ram_[ea]); ram_[ea] = v; EXPR; break;         \
    case (B) + 0x0F: ea = OT_ABS(); v = FN(OT_RD(ea)); OT_WR(ea, v); EXPR; break;       \
    case (B) + 0x13: ea = indY(false); v = FN(OT_RD(ea)); OT_WR(ea, v); EXPR; break;    \
    case (B) + 0x17: ea = OT_ZPX(); v = FN(ram_[ea]); ram_[ea] = v; EXPR; break;        \
    case (B) + 0x1B: ea = absIdx(y, false); v = FN(OT_RD(ea)); OT_WR(ea, v); EXPR; break; \
    case (B) + 0x1F: ea = absIdx(x, false); v = FN(OT_RD(ea)); OT_WR(ea, v); EXPR; break;

            switch (op) {
                OT_GROUP_READ(0x00, a |= v; OT_ZN(a))
                OT_GROUP_READ(0x20, a &= v; OT_ZN(a))
                OT_GROUP_READ(0x40, a ^= v; OT_ZN(a))
                OT_GROUP_READ(0x60, adc(v))
                OT_GROUP_READ(0xA0, a = v; OT_ZN(a))
                OT_GROUP_READ(0xC0, cmp(a, v))
                OT_GROUP_READ(0xE0, adc(uint8_t(~v)))
                case 0xEB: adc(uint8_t(~OT_RD(OT_IMM()))); break;   // SBC #imm (unofficial)

                // STA / STX / STY
                case 0x81: OT_WR(indX(), a); break;
                case 0x85: ram_[OT_ZP()] = a; break;
                case 0x8D: OT_WR(OT_ABS(), a); break;
                case 0x91: OT_WR(indY(false), a); break;
                case 0x95: ram_[OT_ZPX()] = a; break;
                case 0x99: OT_WR(absIdx(y, false), a); break;
                case 0x9D: OT_WR(absIdx(x, false), a); break;
                case 0x86: ram_[OT_ZP()] = x; break;
                case 0x8E: OT_WR(OT_ABS(), x); break;
                case 0x96: ram_[OT_ZPY()] = x; break;
                case 0x84: ram_[OT_ZP()] = y; break;
                case 0x8C: OT_WR(OT_ABS(), y); break;
                case 0x94: ram_[OT_ZPX()] = y; break;
                // LDX / LDY
                case 0xA2: x = OT_RD(OT_IMM()); OT_ZN(x); break;
                case 0xA6: x = ram_[OT_ZP()]; OT_ZN(x); break;
                case 0xAE: x = OT_RD(OT_ABS()); OT_ZN(x); break;
                case 0xB6: x = ram_[OT_ZPY()]; OT_ZN(x); break;
                case 0xBE: x = OT_RD(absIdx(y, true)); OT_ZN(x); break;
                case 0xA0: y = OT_RD(OT_IMM()); OT_ZN(y); break;
                case 0xA4: y = ram_[OT_ZP()]; OT_ZN(y); break;
                case 0xAC: y = OT_RD(OT_ABS()); OT_ZN(y); break;
                case 0xB4: y = ram_[OT_ZPX()]; OT_ZN(y); break;
                case 0xBC: y = OT_RD(absIdx(x, true)); OT_ZN(y); break;
                // CPX / CPY / BIT
                case 0xE0: cmp(x, OT_RD(OT_IMM())); break;
                case 0xE4: cmp(x, ram_[OT_ZP()]); break;
                case 0xEC: cmp(x, OT_RD(OT_ABS())); break;
                case 0xC0: cmp(y, OT_RD(OT_IMM())); break;
                case 0xC4: cmp(y, ram_[OT_ZP()]); break;
                case 0xCC: cmp(y, OT_RD(OT_ABS())); break;
                case 0x24: v = ram_[OT_ZP()]; zr = a & v; nr = v; fv = v & 0x40; break;
                case 0x2C: v = OT_RD(OT_ABS()); zr = a & v; nr = v; fv = v & 0x40; break;

                // Shifts and INC/DEC
                case 0x0A: a = asl(a); break;
                case 0x2A: a = rol(a); break;
                case 0x4A: a = lsr(a); break;
                case 0x6A: a = ror(a); break;
                OT_GROUP_RMW(0x00, asl)
                OT_GROUP_RMW(0x20, rol)
                OT_GROUP_RMW(0x40, lsr)
                OT_GROUP_RMW(0x60, ror)
                OT_GROUP_RMW(0xC0, dec)
                OT_GROUP_RMW(0xE0, inc)
                case 0xCA: --x; OT_ZN(x); break;
                case 0x88: --y; OT_ZN(y); break;
                case 0xE8: ++x; OT_ZN(x); break;
                case 0xC8: ++y; OT_ZN(y); break;

                // Transfers
                case 0xAA: x = a; OT_ZN(x); break;
                case 0xA8: y = a; OT_ZN(y); break;
                case 0x8A: a = x; OT_ZN(a); break;
                case 0x98: a = y; OT_ZN(a); break;
                case 0xBA: x = s; OT_ZN(x); break;
                case 0x9A: s = x; break;

                // Stack
                case 0x48: OT_PUSH(a); break;
                case 0x08: OT_PUSH(OT_P(true)); break;
                case 0x68: a = OT_POP(); OT_ZN(a); break;
                case 0x28: OT_SETP(OT_POP()); break;

                // Flags
                case 0x18: fc = false; break;
                case 0x38: fc = true; break;
                case 0x58: fi = false; break;
                case 0x78: fi = true; break;
                case 0xB8: fv = false; break;
                case 0xD8: fd = false; break;
                case 0xF8: fd = true; break;

                // Branches
                case 0x10: branch(!(nr & 0x80)); break;
                case 0x30: branch(nr & 0x80); break;
                case 0x50: branch(!fv); break;
                case 0x70: branch(fv); break;
                case 0x90: branch(!fc); break;
                case 0xB0: branch(fc); break;
                case 0xD0: branch(zr != 0); break;
                case 0xF0: branch(zr == 0); break;

                // Jumps
                case 0x4C: pc = OT_ABS(); break;
                case 0x6C: {
                    const uint16_t p = OT_ABS();   // page-wrap bug of JMP ($xxFF)
                    pc = uint16_t(OT_RD(p) | (OT_RD(uint16_t((p & 0xFF00) | uint8_t(p + 1))) << 8));
                    break;
                }
                case 0x20: {
                    const uint16_t to = OT_ABS();
                    const uint16_t ret = uint16_t(pc - 1);
                    OT_PUSH(uint8_t(ret >> 8));
                    OT_PUSH(uint8_t(ret));
                    pc = to;
                    break;
                }
                case 0x60: {
                    const uint8_t lo = OT_POP();
                    pc = uint16_t(((OT_POP() << 8) | lo) + 1);
                    break;
                }
                case 0x40: {
                    OT_SETP(OT_POP());
                    const uint8_t lo = OT_POP();
                    pc = uint16_t((OT_POP() << 8) | lo);
                    break;
                }
                case 0x00: {
                    ++pc;
                    OT_PUSH(uint8_t(pc >> 8));
                    OT_PUSH(uint8_t(pc));
                    OT_PUSH(OT_P(true));
                    fi = true;
                    pc = uint16_t(OT_RD(0xFFFE) | (OT_RD(0xFFFF) << 8));
                    break;
                }

                // NOPs (official and unofficial)
                case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
                case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: ++pc; break;
                case 0x04: case 0x44: case 0x64: ++pc; break;
                case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: ++pc; break;
                case 0x0C: pc = uint16_t(pc + 2); break;
                case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
                    (void)OT_RD(absIdx(x, true));
                    break;

                // Unofficial combined opcodes
                OT_GROUP_COMBO(0x00, asl, a |= v; OT_ZN(a))       // SLO
                OT_GROUP_COMBO(0x20, rol, a &= v; OT_ZN(a))       // RLA
                OT_GROUP_COMBO(0x40, lsr, a ^= v; OT_ZN(a))       // SRE
                OT_GROUP_COMBO(0x60, ror, adc(v))                 // RRA
                OT_GROUP_COMBO(0xC0, decq, cmp(a, v))             // DCP
                OT_GROUP_COMBO(0xE0, incq, adc(uint8_t(~v)))      // ISC
                // LAX
                case 0xA3: a = x = OT_RD(indX()); OT_ZN(a); break;
                case 0xA7: a = x = ram_[OT_ZP()]; OT_ZN(a); break;
                case 0xAB: a = x = OT_RD(OT_IMM()); OT_ZN(a); break;
                case 0xAF: a = x = OT_RD(OT_ABS()); OT_ZN(a); break;
                case 0xB3: a = x = OT_RD(indY(true)); OT_ZN(a); break;
                case 0xB7: a = x = ram_[OT_ZPY()]; OT_ZN(a); break;
                case 0xBF: a = x = OT_RD(absIdx(y, true)); OT_ZN(a); break;
                // SAX
                case 0x83: OT_WR(indX(), uint8_t(a & x)); break;
                case 0x87: ram_[OT_ZP()] = uint8_t(a & x); break;
                case 0x8F: OT_WR(OT_ABS(), uint8_t(a & x)); break;
                case 0x97: ram_[OT_ZPY()] = uint8_t(a & x); break;
                // Immediate unofficials
                case 0x0B: case 0x2B: a &= OT_RD(OT_IMM()); OT_ZN(a); fc = a & 0x80; break;   // ANC
                case 0x4B: a &= OT_RD(OT_IMM()); a = lsr(a); break;                           // ALR
                case 0x6B:                                                                    // ARR
                    a &= OT_RD(OT_IMM());
                    a = uint8_t((a >> 1) | (fc ? 0x80 : 0));
                    OT_ZN(a);
                    fc = a & 0x40;
                    fv = ((a >> 6) ^ (a >> 5)) & 1;
                    break;
                case 0xCB: {                                                                  // AXS
                    const uint8_t m = OT_RD(OT_IMM());
                    const uint8_t ax = uint8_t(a & x);
                    fc = ax >= m;
                    x = uint8_t(ax - m);
                    OT_ZN(x);
                    break;
                }
                case 0x8B: a = uint8_t(x & OT_RD(OT_IMM())); OT_ZN(a); break;                 // XAA (approx.)
                case 0xBB: v = uint8_t(OT_RD(absIdx(y, true)) & s); a = x = s = v; OT_ZN(v); break;  // LAS
                case 0x9C: storeHi(OT_ABS(), x, y); break;                                    // SHY
                case 0x9E: storeHi(OT_ABS(), y, x); break;                                    // SHX
                case 0x93: {                                                                  // AHX (zp),Y
                    const uint8_t z = OT_RD(pc++);
                    storeHi(uint16_t(ram_[z] | (ram_[uint8_t(z + 1)] << 8)), y, uint8_t(a & x));
                    break;
                }
                case 0x9F: storeHi(OT_ABS(), y, uint8_t(a & x)); break;                       // AHX abs,Y
                case 0x9B: s = uint8_t(a & x); storeHi(OT_ABS(), y, s); break;                // TAS

                default:
                    // JAM (x2 opcodes): the CPU stops until reset.
                    jam_ = true;
                    pc = uint16_t(pc - 1);
                    break;
            }
        }
        if (dmaStall_) {
            n += dmaStall_;
            dmaStall_ = 0;
        }
        dot += 3 * n;
        cyc += uint64_t(n);
        if (jam_) {
            if (dot < target) dot = target;
            break;
        }
    }
#undef OT_GROUP_READ
#undef OT_GROUP_RMW
#undef OT_GROUP_COMBO
#undef OT_RD
#undef OT_WR
#undef OT_ZN
#undef OT_P
#undef OT_SETP
#undef OT_PUSH
#undef OT_POP
#undef OT_IMM
#undef OT_ZP
#undef OT_ZPX
#undef OT_ZPY
#undef OT_ABS
    pc_ = pc;
    a_ = a;
    x_ = x;
    y_ = y;
    s_ = s;
    fc_ = fc;
    fv_ = fv;
    fi_ = fi;
    fd_ = fd;
    fz_ = zr == 0;
    fn_ = nr & 0x80;
    dot_ = dot;
    cycles_ = cyc;
}

}  // namespace games
}  // namespace ot
