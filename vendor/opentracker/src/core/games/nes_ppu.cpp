// OpenTracker NES core: PPU (RP2C02) registers and the scanline renderer.
// Register semantics (loopy v/t/x/w scroll model) per the public NESdev PPU
// documentation; rendering is line-based (see nes.h).
#include <cstring>

#include "games/nes.h"

namespace ot {
namespace games {

// ---------------------------------------------------------------------------
// VRAM

uint8_t Nes::vramRead(uint16_t a) const {
    a &= 0x3FFF;
    if (a < 0x2000) return chr_[a >> 10][a & 0x3FF];
    if (a < 0x3F00) return nt_[(a >> 10) & 3][a & 0x3FF];
    uint8_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;   // $3F10/14/18/1C mirror $3F00/04/08/0C
    return pal_[i];
}

void Nes::vramWrite(uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000) {
        if (chrIsRam_) chr_[a >> 10][a & 0x3FF] = v;
    } else if (a < 0x3F00) {
        nt_[(a >> 10) & 3][a & 0x3FF] = v;
    } else {
        uint8_t i = a & 0x1F;
        if ((i & 0x13) == 0x10) i &= 0x0F;
        pal_[i] = v & 0x3F;
        colorDirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// Registers $2000-$2007

uint8_t Nes::ppuRead(uint16_t reg) {
    switch (reg) {
        case 2: {
            const uint8_t r = uint8_t((status_ & 0xE0) | (bus_ & 0x1F));
            status_ &= 0x7F;   // reading clears vblank
            w_ = false;
            bus_ = r;
            return r;
        }
        case 4:
            bus_ = oam_[oamAddr_];
            return bus_;
        case 7: {
            a12(v_);
            uint8_t r;
            if ((v_ & 0x3FFF) < 0x3F00) {
                r = readBuf_;
                readBuf_ = vramRead(v_);
            } else {
                r = uint8_t((vramRead(v_) & 0x3F) | (bus_ & 0xC0));
                readBuf_ = vramRead(uint16_t(v_ - 0x1000));   // nametable "under" the palette
            }
            v_ = uint16_t((v_ + ((ctrl_ & 0x04) ? 32 : 1)) & 0x7FFF);
            bus_ = r;
            return r;
        }
        default:
            return bus_;
    }
}

void Nes::ppuWrite(uint16_t reg, uint8_t v) {
    bus_ = v;
    switch (reg) {
        case 0: {
            const bool nmiWasOn = ctrl_ & 0x80;
            ctrl_ = v;
            t_ = uint16_t((t_ & 0xF3FF) | ((v & 3) << 10));
            // Enabling NMI during vblank raises it immediately.
            if (!nmiWasOn && (v & 0x80) && (status_ & 0x80)) nmi_ = true;
            break;
        }
        case 1:
            if ((mask_ ^ v) & 1) colorDirty_ = true;   // grayscale
            mask_ = v;
            break;
        case 3: oamAddr_ = v; break;
        case 4:
            oam_[oamAddr_++] = v;
            oamDirty_ = true;
            break;
        case 5:
            if (!w_) {
                t_ = uint16_t((t_ & 0xFFE0) | (v >> 3));
                fineX_ = v & 7;
            } else {
                t_ = uint16_t((t_ & 0x8C1F) | ((v & 0x07) << 12) | ((v & 0xF8) << 2));
            }
            w_ = !w_;
            break;
        case 6:
            if (!w_) {
                t_ = uint16_t((t_ & 0x00FF) | ((v & 0x3F) << 8));
            } else {
                t_ = uint16_t((t_ & 0xFF00) | v);
                v_ = t_;
                a12(v_);
            }
            w_ = !w_;
            break;
        case 7:
            a12(v_);
            vramWrite(v_, v);
            v_ = uint16_t((v_ + ((ctrl_ & 0x04) ? 32 : 1)) & 0x7FFF);
            break;
        default:
            break;
    }
}

void Nes::a12(uint16_t addr) {
    // PPU address line A12 driven by $2006/$2007 accesses (rendering off):
    // a rising edge clocks the MMC3 counter. While rendering, mapperLine()
    // stands in for the per-line sprite-fetch edge.
    const bool hi = addr & 0x1000;
    if (hi && !a12_ && !renderingOn()) mapperLine();
    a12_ = hi;
}

void Nes::incY() {
    if ((v_ & 0x7000) != 0x7000) {
        v_ = uint16_t(v_ + 0x1000);
        return;
    }
    v_ &= 0x0FFF;
    int y = (v_ & 0x03E0) >> 5;
    if (y == 29) {
        y = 0;
        v_ ^= 0x0800;
    } else if (y == 31) {
        y = 0;
    } else {
        ++y;
    }
    v_ = uint16_t((v_ & ~0x03E0) | (y << 5));
}

// ---------------------------------------------------------------------------
// Scanline renderer

namespace {
// Byte -> 8 bytes, byte k = bit (7 - k): one tile row plane as 8 pixels.
struct SpreadTable {
    uint64_t v[256];
    constexpr SpreadTable() : v() {
        for (int b = 0; b < 256; ++b) {
            uint64_t r = 0;
            for (int k = 0; k < 8; ++k)
                if (b & (0x80 >> k)) r |= uint64_t(1) << (8 * k);
            v[b] = r;
        }
    }
};
constexpr SpreadTable kSpread{};
constexpr uint64_t kOnes = 0x0101010101010101ull;
}  // namespace

void Nes::refreshColors() {
    // Output byte for each palette RAM index (0 = backdrop), with grayscale.
    const uint8_t gray = (mask_ & 1) ? 0x30 : 0x3F;
    for (int i = 0; i < 32; ++i) color_[i] = lut_[pal_[(i & 3) ? i : 0] & gray];
    colorDirty_ = false;
}

void Nes::commitLine(int line) {
    // The frame buffer keeps the last frame: rows that did not change are not
    // rewritten and not marked for upload. Word-wise (newlib-nano's memcmp is
    // bytewise); fb_ rows and outLine_ are 4-byte aligned.
    typedef uint32_t __attribute__((may_alias)) Word;
    Word* row = reinterpret_cast<Word*>(fb_ + line * kWidth);
    const Word* src = reinterpret_cast<const Word*>(outLine_);
    int i = 0;
    while (i < kWidth / 4 && row[i] == src[i]) ++i;
    if (i == kWidth / 4) return;
    for (; i < kWidth / 4; ++i) row[i] = src[i];
    dirty_[line >> 5] |= 1u << (line & 31);
}

void Nes::markAllDirty() {
    for (uint32_t& d : dirty_) d = 0xFFFFFFFFu;
}

void Nes::clearDirty() {
    for (uint32_t& d : dirty_) d = 0;
}

void Nes::buildSpriteLines(int spH) {
    // Per visible line, a 64-bit mask of the sprites covering it (rebuilt
    // after OAM writes or a sprite-size change).
    uint64_t* lines = mem_.spriteLines;
    std::memset(lines, 0, kHeight * sizeof(uint64_t));
    for (int i = 0; i < 64; ++i) {
        const int top = oam_[i * 4] + 1;
        const uint64_t bit = uint64_t(1) << i;
        for (int r = 0; r < spH && top + r < kHeight; ++r) lines[top + r] |= bit;
    }
    oamDirty_ = false;
    spHBuilt_ = uint8_t(spH);
}

void Nes::blankLine(int line) {
    // Rendering off: the backdrop colour (or the palette entry v points at).
    uint8_t c = pal_[0];
    if ((v_ & 0x3F00) == 0x3F00) c = vramRead(v_);
    if (mask_ & 1) c &= 0x30;
    std::memset(outLine_, lut_[c & 0x3F], kWidth);
    commitLine(line);
}

int Nes::renderLine(int line) {
    const bool showBg = mask_ & 0x08, showSp = mask_ & 0x10;
    const int spH = (ctrl_ & 0x20) ? 16 : 8;
    if (oamDirty_ || spHBuilt_ != spH) buildSpriteLines(spH);
    uint64_t cover = mem_.spriteLines[line];
    const bool needHit = showBg && showSp && !(status_ & 0x40) && (cover & 1);

    // Sprite evaluation: the first 8 sprites covering this line (OAM order);
    // a ninth sets the overflow flag (simplified: no hardware evaluation bug).
    uint8_t found[8];
    int count = 0;
    while (cover && count < 8) {
        found[count++] = uint8_t(__builtin_ctzll(cover));
        cover &= cover - 1;
    }
    if (cover) status_ |= 0x20;
    if (!fb_ && !needHit) return -1;   // frame skip: nothing else to compute on this line

    // Background: 33 tiles from the scroll position in v, written 8 pixels at
    // a time at bgLine_[8 - fineX ..]; pixel x is bgLine_[8 + x].
    uint8_t* const bg = bgLine_ + 8;
    if (showBg) {
        uint16_t v = v_;
        const uint16_t table = (ctrl_ & 0x10) ? 0x1000 : 0;
        const int fineY = (v >> 12) & 7;
        uint8_t* dst = bgLine_ + 8 - fineX_;
        for (int tile = 0; tile < 33; ++tile, dst += 8) {
            const uint8_t* nt = nt_[(v >> 10) & 3];
            const int cx = v & 0x1F, cy = (v >> 5) & 0x1F;
            const unsigned pat = table + nt[cy * 32 + cx] * 16u + unsigned(fineY);
            const uint8_t* chr = chr_[pat >> 10] + (pat & 0x3FF);   // +8 stays inside the 1 KB bank
            const uint8_t at = nt[0x3C0 + (cy >> 2) * 8 + (cx >> 2)];
            const uint64_t palHi = uint64_t(((at >> (((cy & 2) << 1) | (cx & 2))) & 3) << 2);
            uint64_t p = kSpread.v[chr[0]] | (kSpread.v[chr[8]] << 1);
            p |= ((p | (p >> 1)) & kOnes) * palHi;
            std::memcpy(dst, &p, 8);
            // Coarse X increment with horizontal nametable wrap.
            if ((v & 0x1F) == 31) v = uint16_t((v & ~0x1F) ^ 0x0400);
            else ++v;
        }
        if (!(mask_ & 0x02)) std::memset(bg, 0, 8);
    } else {
        std::memset(bg, 0, kWidth);
    }

    // Sprites: lower OAM index wins; bit 7 = sprite 0, bit 6 = behind background.
    uint8_t* const sp = spLine_;
    int spMin = kWidth, spMax = 0;
    if (showSp && count) {
        for (int k = 0; k < count; ++k) {
            const int x0 = oam_[found[k] * 4 + 3];
            if (x0 < spMin) spMin = x0;
            if (x0 + 8 > spMax) spMax = x0 + 8 > kWidth ? kWidth : x0 + 8;
        }
        if (spMax > spMin) std::memset(sp + spMin, 0, size_t(spMax - spMin));
        for (int k = count - 1; k >= 0; --k) {   // reverse order: the lower OAM index wins
            const uint8_t* o = &oam_[found[k] * 4];
            int row = line - int(o[0]) - 1;
            const uint8_t attr = o[2];
            if (attr & 0x80) row = spH - 1 - row;
            unsigned pat;
            if (spH == 16) {
                const uint8_t tileNum = o[1];
                pat = ((tileNum & 1) ? 0x1000u : 0u) + (tileNum & 0xFEu) * 16u + (row >= 8 ? 16u : 0u) + unsigned(row & 7);
            } else {
                pat = ((ctrl_ & 0x08) ? 0x1000u : 0u) + o[1] * 16u + unsigned(row);
            }
            const uint8_t* chr = chr_[pat >> 10] + (pat & 0x3FF);
            const uint8_t lo = chr[0], hi = chr[8];
            if (!(lo | hi)) continue;
            const int x0 = o[3];
            const int w = x0 + 8 > kWidth ? kWidth - x0 : 8;
            const uint8_t tag = uint8_t(0x10 | ((attr & 3) << 2) | ((attr & 0x20) ? 0x40 : 0) | (found[k] == 0 ? 0x80 : 0));
            for (int b = 0; b < w; ++b) {
                const int bit = (attr & 0x40) ? b : 7 - b;
                const int px = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (px) sp[x0 + b] = uint8_t(tag | px);
            }
        }
        if (!(mask_ & 0x04) && spMin < 8) std::memset(sp + spMin, 0, size_t((spMax < 8 ? spMax : 8) - spMin));
    }

    // Sprite-0 hit: first x where an opaque sprite-0 pixel meets an opaque
    // background pixel (never at x = 255).
    int hit = -1;
    if (needHit && spMin < spMax) {
        const int x1 = spMax < 255 ? spMax : 255;
        for (int x = spMin; x < x1; ++x) {
            if ((sp[x] & 0x80) && bg[x]) {
                hit = x;
                break;
            }
        }
    }
    if (!fb_) return hit;

    // Compose: background through the colour cache, then sprite pixels on top
    // (in front, or behind a transparent background pixel).
    if (colorDirty_) refreshColors();
    uint8_t* out = outLine_;
    const uint8_t* col = color_;
    for (int x = 0; x < kWidth; x += 4) {
        out[x] = col[bg[x]];
        out[x + 1] = col[bg[x + 1]];
        out[x + 2] = col[bg[x + 2]];
        out[x + 3] = col[bg[x + 3]];
    }
    for (int x = spMin; x < spMax; ++x) {
        const uint8_t s = sp[x];
        if (s && (!(s & 0x40) || !bg[x])) out[x] = col[s & 0x1F];
    }
    commitLine(line);
    return hit;
}

}  // namespace games
}  // namespace ot
