// OpenTracker NES core: frame loop (NTSC timing) and the colour palette.
#include <cmath>

#include "games/nes.h"

namespace ot {
namespace games {

namespace {
constexpr int32_t kDotsPerLine = 341;
constexpr int kLines = 262;          // 240 visible, 241 vblank start, 261 pre-render
constexpr int kVblankLine = 241;
constexpr int kPreLine = 261;
// NES colour hue alignment (degrees per step 30; tuned so 0x01 is blue,
// 0x06 red and 0x0A green).
constexpr float kHueOffset = 3.9f;
}  // namespace

void Nes::setPalette(const uint8_t lut[64]) {
    for (int i = 0; i < 64; ++i) lut_[i] = lut[i];
    colorDirty_ = true;
}

bool Nes::runFrame(uint8_t* fb) {
    if (!loaded_ || jam_) return false;
    fb_ = fb;
    audioCount_ = 0;
    int32_t frameDots = kLines * kDotsPerLine;
    for (int line = 0; line < kLines && !jam_; ++line) {
        const int32_t base = line * kDotsPerLine;
        if (line < 240) {
            int hit = -1;
            if (renderingOn()) hit = renderLine(line);
            else if (fb_) blankLine(line);
            if (hit >= 0) {
                runTo(base + hit + 2);   // pixel x is output at dot x + 1
                status_ |= 0x40;
            }
            runTo(base + 257);
            if (renderingOn()) {
                incY();
                copyX();
            }
            runTo(base + 260);
            if (renderingOn()) mapperLine();
            runTo(base + kDotsPerLine);
        } else if (line == kVblankLine) {
            status_ |= 0x80;
            if (ctrl_ & 0x80) nmi_ = true;
            runTo(base + kDotsPerLine);
        } else if (line == kPreLine) {
            status_ &= 0x1F;   // clear vblank, sprite 0, overflow
            runTo(base + 257);
            if (renderingOn()) copyX();
            runTo(base + 260);
            if (renderingOn()) mapperLine();
            runTo(base + 304);
            if (renderingOn()) copyY();
            // Odd frames with rendering on skip the last dot of the pre-render line.
            if (odd_ && renderingOn()) frameDots -= 1;
            runTo(frameDots);
        } else {
            runTo(base + kDotsPerLine);
        }
        apuSync();
    }
    dot_ -= frameDots;
    odd_ = !odd_;
    ++frames_;
    fb_ = nullptr;
    return !jam_;
}

uint32_t Nes::nesColor(uint8_t index) {
    // Composite signal model of the 2C02: each colour is a square wave between
    // two voltage levels at one of 12 phases (public NTSC/2C02 measurements).
    static const float kLo[4] = {0.350f, 0.518f, 0.962f, 1.550f};
    static const float kHi[4] = {1.094f, 1.506f, 1.962f, 1.962f};
    constexpr float kBlack = 0.518f, kWhite = 1.962f, kPi = 3.14159265f;
    const int hue = index & 0x0F, lum = (index >> 4) & 3;
    float lo = kLo[lum], hi = kHi[lum];
    if (hue == 0) lo = hi;
    else if (hue == 0x0D) hi = lo;
    else if (hue > 0x0D) lo = hi = kBlack;
    float y = 0, i = 0, q = 0;
    for (int p = 0; p < 12; ++p) {
        const bool high = (hue + p) % 12 < 6;
        const float s = ((high ? hi : lo) - kBlack) / (kWhite - kBlack);
        const float ang = kPi * (float(p) + kHueOffset) / 6.0f;
        y += s;
        i += s * std::cos(ang);
        q += s * std::sin(ang);
    }
    y /= 12.0f;
    i /= 12.0f;
    q /= 12.0f;
    const float rgb[3] = {y + 0.946882f * i + 0.623557f * q, y - 0.274788f * i - 0.635691f * q,
                          y - 1.108545f * i + 1.709007f * q};
    uint32_t out = 0;
    for (float c : rgb) {
        c = c < 0 ? 0 : (c > 1 ? 1 : c);
        out = (out << 8) | uint32_t(c * 255.0f + 0.5f);
    }
    return out;
}

void Nes::rgb332Lut(uint8_t out[64]) {
    // spec 14 §3.1: (R & 0xE0) | ((G >> 5) << 2) | (B >> 6).
    for (int k = 0; k < 64; ++k) {
        const uint32_t c = nesColor(uint8_t(k));
        const uint8_t r = uint8_t(c >> 16), g = uint8_t(c >> 8), b = uint8_t(c);
        out[k] = uint8_t((r & 0xE0) | ((g >> 5) << 2) | (b >> 6));
    }
}

}  // namespace games
}  // namespace ot
