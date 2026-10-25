// Generated UI fonts (src/core/ui/fonts, tools/fonts/gen_fonts.py) against
// spec 20 §2.2 (cell, baseline, cap / x-height, descender: measured on the
// glyph ink, +-1 px) and §2.3 (pen advance tables, exact), plus the RAM_G
// image layout the FT81x driver and the simulator rely on.
#include "test.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "formats/crc32.h"
#include "ui/fonts/fonts.h"
#include "ui/gfx.h"
#include "ui/widgets.h"

using namespace ot;
namespace fd = ot::fontdata;

namespace {

struct Expect {
    int w, h, stride, first, base, cap, xh, desc;
};
// spec 20 §2.2
const Expect kSpec[2][5] = {
    {{10, 18, 5, 32, 15, 11, 8, 3}, {10, 22, 5, 32, 18, 12, 9, 3}, {10, 19, 5, 32, 15, 11, 8, 3},
     {12, 22, 6, 24, 18, 12, 9, 3}, {8, 16, 4, 17, 13, 9, 7, 3}},
    {{14, 20, 7, 32, 17, 10, 7, 3}, {16, 24, 8, 32, 20, 12, 9, 3}, {14, 20, 7, 32, 17, 10, 7, 3},
     {18, 24, 9, 24, 20, 12, 9, 3}, {12, 17, 6, 17, 14, 9, 6, 3}},
};

// spec 20 §2.3, notation as printed there.
const char* const kSet0F3 =
    " sp 8  !8 \"8 #9 $8 %9 &9 '8 (8 )8 *8 +8 ,8 -8 .8 /8 0-9:8 :8 ;8 <8 =8 >8 ?8 @8"
    " A9 B8 C8 D8 E7 F7 G8 H8 I7 J8 K9 L7 M8 N8 O8 P8 Q8 R7 S8 T8 U9 V8 W9 X8 Y8 Z8"
    " [8 \\8 ]8 ^8 _8 `8 a7 b8 c7 d8 e8 f7 g8 h8 i7 j6 k8 l7 m9 n7 o8 p7 q8 r6 s7 t7"
    " u7 v8 w9 x8 y8 z8 {8 |8 }8 ~9";
const char* const kSet1[5] = {
    " sp3 !4 \"5 #9 $8 %12 &11 '3 (4 )4 *6 +8 ,3 -5 .4 /6 0 8 1 6 2-9 8 :3 ;3 <8 =8 >8 ?7 @12"
    " A10 B9 C10 D10 E8 F8 G10 H10 I4 J5 K9 L8 M12 N11 O11 P9 Q11 R8 S9 T9 U9 V9 W13 X9 Y9 Z8"
    " [4 \\6 ]4 ^7 _7 `4 a8 b8 c8 d8 e8 f5 g8 h7 i3 j5 k7 l3 m11 n7 o8 p7 q8 r5 s7 t5 u7 v8 w11"
    " x8 y9 z7 {4 |4 }4 ~8",
    " sp4 !5 \"6 #11 $10 %15 &13 '4 (5 )5 *7 +10 ,4 -5 .4 /8 0-9 10 :4 ;4 <10 =10 >10 ?9 @15"
    " A12 B11 C12 D12 E11 F10 G12 H13 I5 J6 K11 L10 M16 N13 O12 P10 Q12 R11 S10 T10 U12 V11 W16"
    " X11 Y11 Z10 [5 \\8 ]5 ^8 _9 `5 a9 b10 c9 d10 e9 f5 g10 h10 i4 j5 k9 l4 m14 n9 o10 p10 q10"
    " r6 s8 t5 u10 v10 w13 x10 y10 z8 {5 |5 }5 ~10",
    " sp3 !5 \"6 #9 $8 %13 &11 '4 (5 )5 *6 +8 ,4 -5 .4 /7 0-3 8 4 9 5-9 8 :4 ;4 <8 =8 >8 ?8 @12"
    " A10 B9 C10 D10 E8 F8 G10 H11 I4 J5 K10 L8 M13 N11 O10 P8 Q10 R9 S9 T9 U9 V9 W14 X9 Y9 Z8"
    " [5 \\7 ]5 ^7 _7 `5 a8 b8 c8 d8 e8 f5 g8 h8 i4 j5 k8 l4 m13 n8 o8 p8 q8 r6 s7 t5 u8 v9 w12"
    " x9 y9 z7 {5 |4 }5 ~8",
    " sp4 !6 \"7 #11 $10 %15 &13 '4 (6 )6 *7 +10 ,4 -6 .5 /8 0-9 11 :4 ;4 <10 =10 >10 ?9 @15"
    " A11 B11 C11 D11 E11 F11 G13 H12 I5 J7 K11 L10 M16 N13 O13 P10 Q13 R12 S11 T11 U12 V11 W17"
    " X11 Y11 Z10 [6 \\8 ]6 ^9 _9 `6 a9 b10 c10 d10 e10 f6 g10 h9 i4 j6 k9 l4 m14 n9 o10 p10 q10"
    " r7 s9 t7 u10 v10 w14 x10 y10 z8 {6 |5 }6 ~10",
    " sp3 !3 \"4 #8 $7 %10 &9 '3 (4 )4 *5 +7 ,3 -4 .3 /5 0-9 7 :3 ;3 <7 =7 >7 ?6 @10"
    " A9 B8 C8 D9 E7 F7 G9 H9 I3 J4 K8 L7 M11 N9 O9 P8 Q9 R8 S7 T8 U8 V8 W11 X8 Y8 Z7"
    " [4 \\6 ]4 ^6 _6 `4 a6 b7 c6 d7 e7 f4 g7 h7 i3 j4 k7 l3 m10 n7 o7 p7 q7 r5 s6 t4 u7 v7 w10"
    " x7 y7 z6 {4 |3 }4 ~7",
};

bool isNum(const std::string& t) {
    if (t.empty()) return false;
    for (char c : t)
        if (!std::isdigit(uint8_t(c))) return false;
    return true;
}

// Notation -> advances[128] (0 = unset). Tokens: "sp N" / "spN", "a-b N" /
// "a-b:N", "c N" (single char then number), "cN".
std::vector<int> parseTable(const char* text) {
    std::vector<std::string> tok;
    std::string cur;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') {
            if (!cur.empty()) tok.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) tok.push_back(cur);
    std::vector<int> adv(128, 0);
    for (size_t i = 0; i < tok.size(); ++i) {
        const std::string& t = tok[i];
        if (t.rfind("sp", 0) == 0) {
            adv[32] = t.size() == 2 ? std::stoi(tok[++i]) : std::stoi(t.substr(2));
        } else if (t.size() >= 3 && t[1] == '-') {
            const std::string rest = t.substr(3);
            const int n = rest.empty() ? std::stoi(tok[++i]) : std::stoi(rest.substr(1));
            for (int c = uint8_t(t[0]); c <= uint8_t(t[2]); ++c) adv[c] = n;
        } else if (t.size() == 1 && i + 1 < tok.size() && isNum(tok[i + 1])) {
            adv[uint8_t(t[0])] = std::stoi(tok[++i]);
        } else {
            adv[uint8_t(t[0])] = std::stoi(t.substr(1));
        }
    }
    return adv;
}

std::vector<int> expectedAdvances(int set, int font) {
    std::vector<int> a(128, 0);
    if (set == 0) {
        if (font == 3) {
            a = parseTable(kSet0F3);
        } else {
            for (int c = 32; c < 127; ++c) {
                const char ch = char(c);
                if (font == 1) a[c] = std::strchr("#%&KWw", ch) ? 9 : 8;
                if (font == 2) a[c] = 10;
                if (font == 4) a[c] = std::strchr("&AKQWXYy", ch) ? 11 : 10;
                if (font == 5) a[c] = c == 32 ? 4 : 7;
            }
        }
        if (font == 4) for (int c = 24; c < 32; ++c) a[c] = 12;
        if (font == 5) for (int c = 17; c < 32; ++c) a[c] = 8;
    } else {
        a = parseTable(kSet1[font - 1]);
        if (font == 4) for (int c = 24; c < 32; ++c) a[c] = 18;
        if (font == 5) for (int c = 17; c < 32; ++c) a[c] = 7;
    }
    return a;
}

int nib(const fd::FontInfo& f, uint8_t ch, int x, int y) {
    const uint8_t* c = fd::cell(f, ch);
    const uint8_t b = c[y * f.stride + x / 2];
    return (x & 1) ? (b & 15) : (b >> 4);
}

constexpr int kInk = 6;   // nibble >= 6 (alpha >= 102) counts as ink

// First / last ink row of a glyph (-1 when empty).
void inkRows(const fd::FontInfo& f, uint8_t ch, int& top, int& bot) {
    top = bot = -1;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (nib(f, ch, x, y) >= kInk) {
                if (top < 0) top = y;
                bot = y;
            }
}
int inkLeft(const fd::FontInfo& f, uint8_t ch) {
    for (int x = 0; x < f.w; ++x)
        for (int y = 0; y < f.h; ++y)
            if (nib(f, ch, x, y) > 0) return x;
    return -1;
}
uint32_t rd32(uint32_t off) {
    return uint32_t(fd::kBlob[off]) | (uint32_t(fd::kBlob[off + 1]) << 8) | (uint32_t(fd::kBlob[off + 2]) << 16) |
           (uint32_t(fd::kBlob[off + 3]) << 24);
}
int absd(int a, int b) { return a > b ? a - b : b - a; }

}  // namespace

TEST(font_table_parser_covers_printable_ascii) {
    for (int s = 0; s < 2; ++s)
        for (int f = 1; f <= 5; ++f) {
            const std::vector<int> a = expectedAdvances(s, f);
            for (int c = 32; c < 127; ++c) CHECK(a[c] > 0);
        }
    CHECK_EQ(expectedAdvances(1, 1)['1'], 6);
    CHECK_EQ(expectedAdvances(1, 3)['4'], 9);
    CHECK_EQ(expectedAdvances(1, 2)['-'], 5);
    CHECK_EQ(expectedAdvances(0, 3)['j'], 6);
}

TEST(font_image_layout_and_metric_blocks) {
    CHECK_EQ(fd::kRamGBase, 0x06CC00u);
    CHECK(fd::kRamGEnd <= 0x0A3930u);
    CHECK_EQ(crc32(fd::kBlob, fd::kBlobSize), fd::kBlobCrc);
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < 5; ++r) {
            const fd::FontInfo& f = fd::info(s, r);
            const Expect& e = kSpec[s][r];
            CHECK_EQ(int(f.w), e.w);
            CHECK_EQ(int(f.h), e.h);
            CHECK_EQ(int(f.stride), e.stride);
            CHECK_EQ(int(f.first), e.first);
            CHECK_EQ(int(f.baseline), e.base);
            CHECK_EQ(int(f.cap), e.cap);
            CHECK_EQ(f.metric % 4, 0u);
            CHECK_EQ(f.glyphs, f.metric + fd::kMetricBytes);
            // Legacy metric block: format L4, stride, width, height, char-0 cell.
            CHECK_EQ(rd32(f.metric + 128), 2u);
            CHECK_EQ(rd32(f.metric + 132), uint32_t(e.stride));
            CHECK_EQ(rd32(f.metric + 136), uint32_t(e.w));
            CHECK_EQ(rd32(f.metric + 140), uint32_t(e.h));
            CHECK_EQ(rd32(f.metric + 144), fd::sourceAddr(f));
            CHECK(f.glyphs + uint32_t(127 - f.first) * f.stride * f.h <= fd::kBlobSize);
        }
    CHECK_EQ(int(fd::kIcons.count), int(kIconCount));
    CHECK_EQ(int(fd::kIcons.w), kIconSize);
    CHECK(fd::kIcons.offset + uint32_t(fd::kIcons.count) * fd::kIcons.stride * fd::kIcons.h <= fd::kBlobSize);
}

TEST(font_advances_match_spec20) {
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < 5; ++r) {
            const fd::FontInfo& f = fd::info(s, r);
            const std::vector<int> want = expectedAdvances(s, r + 1);
            for (int c = 0; c < 128; ++c) {
                const int got = fd::advance(f, uint8_t(c));
                const int exp = (c >= f.first && c < 127) ? want[c] : 0;
                if (got != exp) std::printf("set %d font %d char %d: advance %d, spec %d\n", s, r + 1, c, got, exp);
                CHECK_EQ(got, exp);
            }
        }
}

TEST(font_ink_metrics_within_one_pixel) {
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < 5; ++r) {
            const fd::FontInfo& f = fd::info(s, r);
            const Expect& e = kSpec[s][r];
            int ht, hb, xt, xb, pt, pb;
            inkRows(f, 'H', ht, hb);
            inkRows(f, 'x', xt, xb);
            inkRows(f, 'p', pt, pb);
            const int base = hb + 1, cap = hb - ht + 1, xh = xb - xt + 1, desc = pb + 1 - base;
            std::printf("set %d font %d: baseline %d/%d cap %d/%d x-height %d/%d desc %d/%d lsb %d\n", s, r + 1,
                        base, e.base, cap, e.cap, xh, e.xh, desc, e.desc, inkLeft(f, 'H'));
            CHECK(absd(base, e.base) <= 1);
            CHECK(absd(cap, e.cap) <= 1);
            CHECK(absd(xh, e.xh) <= 1);
            CHECK(absd(desc, e.desc) <= 1);
            CHECK_EQ(xb, hb);   // x and H share the baseline
            // Ink left-aligned in the cell (§2.1), except set 0 fonts 1 / 3
            // (10-px cell, 8-px pen): the glyph sits 1 px right of the pen,
            // as measured on stock bar labels (Snapshot_010 "Play" 36..65 for
            // pen 35) -- pixels win over the spec text.
            const int lsb = (s == 0 && (r == 0 || r == 2)) ? 1 : 0;
            CHECK_EQ(inkLeft(f, 'H'), lsb);
            CHECK_EQ(inkLeft(f, 'M'), lsb);
            // Every printable glyph but the space has ink; symbols too.
            for (int c = f.first; c < 127; ++c) {
                int t, b;
                inkRows(f, uint8_t(c), t, b);
                if (c != ' ') CHECK(t >= 0);
                if (c == ' ') CHECK_EQ(t, -1);
            }
        }
    // Icons: every cell has ink.
    for (int i = 0; i < fd::kIcons.count; ++i) {
        int sum = 0;
        for (int b = 0; b < fd::kIcons.stride * fd::kIcons.h; ++b)
            sum += fd::kBlob[fd::kIcons.offset + uint32_t(i) * fd::kIcons.stride * fd::kIcons.h + b];
        CHECK(sum > 0);
    }
}

namespace {
// Display with custom fonts that logs the bitmap commands.
struct FontDisplay : IDisplay {
    std::vector<std::string> log;
    void add(const char* fmt, long a, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), fmt, a, b, c, d, e, f);
        log.push_back(buf);
    }
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override {}
    void clearColorRGB(uint8_t, uint8_t, uint8_t) override {}
    void clear(bool, bool, bool) override {}
    void colorRGB(uint8_t, uint8_t, uint8_t) override {}
    void colorA(uint8_t) override {}
    void lineWidth(uint16_t) override {}
    void pointSize(uint16_t) override {}
    void scissorXY(uint16_t, uint16_t) override {}
    void scissorSize(uint16_t, uint16_t) override {}
    void begin(Prim) override {}
    void end() override {}
    void vertex2f(int32_t, int32_t) override {}
    void vertex2ii(uint16_t x, uint16_t y, uint8_t h, uint8_t c) override { add("V %ld %ld %ld %ld", x, y, h, c); }
    void text(int16_t, int16_t, uint8_t, uint16_t, const char*) override {}
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t) override {}
    void swap() override {}
    bool bindFontSet(int) override { return true; }
    void saveContext() override { add("SAVE", 0); }
    void restoreContext() override { add("RESTORE", 0); }
    void vertexTranslateX(int32_t x16) override { add("TX %ld", x16); }
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override {
        add("XF %ld %ld %ld %ld %ld %ld", a, b, c, d, e, f);
    }
    void bitmapSize(uint8_t h, uint16_t w, uint16_t hh) override { add("SIZE %ld %ld %ld", h, w, hh); }
};
bool logged(const FontDisplay& d, const char* line) {
    for (const std::string& s : d.log)
        if (s == line) return true;
    return false;
}
}  // namespace

TEST(gfx_metrics_follow_bound_font_set) {
    // Not bound: ROM fallbacks. Bound (a display with custom fonts): the
    // generated advance tables and cell heights.
    FontDisplay d;
    CHECK(applyFontSet(&d, 0));
    CHECK_EQ(textWidth(kFont2, "Pattern 10"), 100);   // spec 20 §2.1: 10 x 10 px
    CHECK_EQ(textWidth(kFont1, "KW"), 18);
    CHECK_EQ(textWidth(kFont4, "AB"), 21);
    CHECK_EQ(fontHeight(kFont4), 22);
    CHECK_EQ(textWidth(kFontIcons, "\x01\x02"), 2 * kIconSize);
    CHECK(applyFontSet(&d, 1));
    CHECK_EQ(textWidth(kFont2, "Wi"), 16 + 4);
    CHECK_EQ(fontHeight(kFont5), 17);
    CHECK_EQ(textWidth(kFontMono, "ABCD"), 32);   // ROM handles unchanged
    applyFontSet(nullptr, 0);
    CHECK_EQ(textWidth(kFont2, "ABCD"), 32);   // back to the ROM fallback
}

TEST(text_rotated_uses_bitmap_transform) {
    FontDisplay d;
    CHECK(applyFontSet(&d, 0));
    Gfx g(d);
    // kFont3 set 0: cell 10 x 19; "AB" advances 9 + 8 = 17. Quarter turn CCW:
    // box 19 wide x 17 tall at (100, 200); "A" at the bottom, "B" above it.
    g.textRotated(100, 200, kFont3, 0xFFFFFF, "AB", 1);
    CHECK(logged(d, "SAVE"));
    CHECK(logged(d, "SIZE 5 19 10"));
    CHECK(logged(d, "XF 0 -256 2432 256 0 0"));
    CHECK(logged(d, "V 100 207 5 65"));   // y = 200 + 17 - 0 - 10
    CHECK(logged(d, "V 100 198 5 66"));   // y = 200 + 17 - 9 - 10
    CHECK(logged(d, "RESTORE"));
    CHECK(d.log.back() == "SIZE 5 10 19");  // cell size restored after the context
    d.log.clear();
    g.textRotated(600, 100, kFont3, 0xFFFFFF, "A", 3);
    CHECK(logged(d, "XF 0 256 0 -256 0 4736"));
    CHECK(logged(d, "TX 6400"));
    CHECK(logged(d, "V 200 100 5 65"));
    // Icons come from the icon sheet when custom fonts are bound.
    d.log.clear();
    g.icon(10, 20, kIconRec, 0xFF0000);
    CHECK(logged(d, "V 10 20 8 3"));
    applyFontSet(nullptr, 0);
}

// The firmware's packed pen advances (font_adv.inc, tools/fonts/pack_fonts.py)
// are the metric width tables of the raw image.
TEST(packed_advances_match_image) {
    static const uint8_t adv[10][128] = {
#include "ui/fonts/font_adv.inc"
    };
    for (int set = 0; set < 2; ++set)
        for (int role = 0; role < 5; ++role)
            for (int ch = 0; ch < 128; ++ch)
                CHECK_EQ(adv[set * 5 + role][ch], fd::kBlob[fd::info(set, role).metric + uint32_t(ch)]);
}

TEST(cpu_cell_label_and_value_do_not_touch_with_custom_fonts) {
    // Device photo: with the custom (wider) font 4 the label was clamped into
    // the 60-px cell and ran into the value ("CPU29%0m:13s"). The cell grows
    // with the text: "CPU" + a space + "nn%" fit before the play time.
    FontDisplay d;
    CHECK(applyFontSet(&d, 0));
    const int w = textWidth(kFont4, "CPU 00%");
    CHECK(wg::cpuCellWidth() >= w);
    wg::CpuCell c = wg::cpuCell(1, true, 290, false, false);
    c.right = int16_t(wg::cpuCellRight(13));
    // Label start + "CPU" + " " ends at or before the value's start.
    const int labelEnd = c.right - textWidth(kFont4, "00%") - textWidth(kFont4, " ");
    CHECK(labelEnd - textWidth(kFont4, "CPU") >= c.right - wg::cpuCellWidth());
    applyFontSet(nullptr, 0);
}
