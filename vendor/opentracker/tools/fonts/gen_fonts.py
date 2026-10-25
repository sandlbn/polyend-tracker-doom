#!/usr/bin/env python3
"""OpenTracker font generator (run by `make fonts`; needs python3 + Pillow).

  gen_fonts.py ui  OUTDIR [--preview PNG]   (writes OUTDIR/font_data.h, font_data.inc)
      The 10 custom UI fonts of spec 20 §2 (two sets x five roles) plus the
      icon sheet, as one FT81x RAM_G image: per font a 148-byte legacy font
      metric block (CMD_SETFONT) followed by L4 glyph cells, then the 12x12 L4
      icon cells. Rendered from the open-licensed fonts in third_party/fonts
      (Roboto Mono: OFL-1.1, Roboto: Apache-2.0). Cell size, baseline, cap
      height and the per-character pen advance follow spec 20 §2.2/§2.3 (the
      advance table is forced, the size is fitted to the cap height, ink is
      left-aligned in the cell). Symbol glyphs (spec 20 §2.4) and icons are our
      own drawings, not copies of any vendor art.

  gen_fonts.py rom SANS.ttf MONO.ttf OUT.inc
      Simulator look-alikes of the FT81x ROM fonts 16..31 (DejaVu, Bitstream
      Vera licence: sim/third_party/DEJAVU_LICENSE.txt).

The output is deterministic for a given Pillow/FreeType version and committed
to the repository, so normal builds do not need Python or the TTFs.
"""
import os
import re
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTDIR = os.path.normpath(os.path.join(HERE, "..", "..", "third_party", "fonts"))

# FT81x RAM_G placement (src/hal/ft81x.h: free band 0x06CC00 .. 0x0A392F).
RAMG_BASE = 0x06CC00
RAMG_END = 0x0A3930
K = 8            # oversampling factor of the rasteriser
L4 = 2           # FT81x bitmap format L4

# ---------------------------------------------------------------------------
# Advance tables, spec 20 §2.3 (notation copied from the spec text).
SET1 = {
    1: """ sp3 !4 "5 #9 $8 %12 &11 '3 (4 )4 *6 +8 ,3 -5 .4 /6 0 8 1 6 2-9 8 :3 ;3 <8 =8 >8 ?7 @12
 A10 B9 C10 D10 E8 F8 G10 H10 I4 J5 K9 L8 M12 N11 O11 P9 Q11 R8 S9 T9 U9 V9 W13 X9 Y9 Z8
 [4 \\6 ]4 ^7 _7 `4 a8 b8 c8 d8 e8 f5 g8 h7 i3 j5 k7 l3 m11 n7 o8 p7 q8 r5 s7 t5 u7 v8 w11
 x8 y9 z7 {4 |4 }4 ~8""",
    2: """ sp4 !5 "6 #11 $10 %15 &13 '4 (5 )5 *7 +10 ,4 -5 .4 /8 0-9 10 :4 ;4 <10 =10 >10 ?9 @15
 A12 B11 C12 D12 E11 F10 G12 H13 I5 J6 K11 L10 M16 N13 O12 P10 Q12 R11 S10 T10 U12 V11 W16
 X11 Y11 Z10 [5 \\8 ]5 ^8 _9 `5 a9 b10 c9 d10 e9 f5 g10 h10 i4 j5 k9 l4 m14 n9 o10 p10 q10
 r6 s8 t5 u10 v10 w13 x10 y10 z8 {5 |5 }5 ~10""",
    3: """ sp3 !5 "6 #9 $8 %13 &11 '4 (5 )5 *6 +8 ,4 -5 .4 /7 0-3 8 4 9 5-9 8 :4 ;4 <8 =8 >8 ?8 @12
 A10 B9 C10 D10 E8 F8 G10 H11 I4 J5 K10 L8 M13 N11 O10 P8 Q10 R9 S9 T9 U9 V9 W14 X9 Y9 Z8
 [5 \\7 ]5 ^7 _7 `5 a8 b8 c8 d8 e8 f5 g8 h8 i4 j5 k8 l4 m13 n8 o8 p8 q8 r6 s7 t5 u8 v9 w12
 x9 y9 z7 {5 |4 }5 ~8""",
    4: """ sp4 !6 "7 #11 $10 %15 &13 '4 (6 )6 *7 +10 ,4 -6 .5 /8 0-9 11 :4 ;4 <10 =10 >10 ?9 @15
 A11 B11 C11 D11 E11 F11 G13 H12 I5 J7 K11 L10 M16 N13 O13 P10 Q13 R12 S11 T11 U12 V11 W17
 X11 Y11 Z10 [6 \\8 ]6 ^9 _9 `6 a9 b10 c10 d10 e10 f6 g10 h9 i4 j6 k9 l4 m14 n9 o10 p10 q10
 r7 s9 t7 u10 v10 w14 x10 y10 z8 {6 |5 }6 ~10""",
    5: """ sp3 !3 "4 #8 $7 %10 &9 '3 (4 )4 *5 +7 ,3 -4 .3 /5 0-9 7 :3 ;3 <7 =7 >7 ?6 @10
 A9 B8 C8 D9 E7 F7 G9 H9 I3 J4 K8 L7 M11 N9 O9 P8 Q9 R8 S7 T8 U8 V8 W11 X8 Y8 Z7
 [4 \\6 ]4 ^6 _6 `4 a6 b7 c6 d7 e7 f4 g7 h7 i3 j4 k7 l3 m10 n7 o7 p7 q7 r5 s6 t4 u7 v7 w10
 x7 y7 z6 {4 |3 }4 ~7""",
}
SET0_F3 = """ sp 8  !8 "8 #9 $8 %9 &9 '8 (8 )8 *8 +8 ,8 -8 .8 /8 0-9:8 :8 ;8 <8 =8 >8 ?8 @8
 A9 B8 C8 D8 E7 F7 G8 H8 I7 J8 K9 L7 M8 N8 O8 P8 Q8 R7 S8 T8 U9 V8 W9 X8 Y8 Z8
 [8 \\8 ]8 ^8 _8 `8 a7 b8 c7 d8 e8 f7 g8 h8 i7 j6 k8 l7 m9 n7 o8 p7 q8 r6 s7 t7
 u7 v8 w9 x8 y8 z8 {8 |8 }8 ~9"""


def parse_table(text):
    """Spec 20 §2.3 notation -> {code: advance} for 32..126."""
    toks = text.split()
    out = {}
    i = 0
    while i < len(toks):
        t = toks[i]
        i += 1
        if t.startswith("sp"):
            if len(t) == 2:
                out[32] = int(toks[i]); i += 1
            else:
                out[32] = int(t[2:])
            continue
        m = re.match(r"^(\S)-(\S)(?::(\d+))?$", t)
        if len(t) >= 3 and m:
            a, b, n = m.group(1), m.group(2), m.group(3)
            if n is None:
                n = toks[i]; i += 1
            for c in range(ord(a), ord(b) + 1):
                out[c] = int(n)
            continue
        if len(t) == 1:
            out[ord(t)] = int(toks[i]); i += 1
            continue
        out[ord(t[0])] = int(t[1:])
    missing = [c for c in range(32, 127) if c not in out]
    if missing:
        raise SystemExit("advance table misses %r" % [chr(c) for c in missing])
    return out


def set0_table(font):
    if font == 1:
        return {c: 9 if chr(c) in "#%&KWw" else 8 for c in range(32, 127)}
    if font == 2:
        return {c: 10 for c in range(32, 127)}
    if font == 3:
        return parse_table(SET0_F3)
    if font == 4:
        return {c: 11 if chr(c) in "&AKQWXYy" else 10 for c in range(32, 127)}
    if font == 5:
        t = {c: 7 for c in range(32, 127)}
        t[32] = 4
        return t
    raise ValueError(font)


# ---------------------------------------------------------------------------
# Font specs, spec 20 §2.2: (cell w, cell h, first char, baseline, cap h,
# face, monospace advance or 0 = proportional, symbol advance).
MONO = {"regular": "RobotoMono-Regular.ttf", "medium": "RobotoMono-Medium.ttf", "bold": "RobotoMono-Bold.ttf"}
SANS = {"light": "Roboto-Light.ttf", "regular": "Roboto-Regular.ttf", "medium": "Roboto-Medium.ttf",
        "bold": "Roboto-Bold.ttf"}
FONTS = [
    # set 0 "Original": monospace
    dict(set=0, font=1, w=10, h=18, first=32, base=15, cap=11, xh=8, face=MONO["regular"], mono=8, centre=True),
    dict(set=0, font=2, w=10, h=22, first=32, base=18, cap=12, xh=9, face=MONO["regular"], mono=10),
    dict(set=0, font=3, w=10, h=19, first=32, base=15, cap=11, xh=8, face=MONO["medium"], mono=8, centre=True),
    dict(set=0, font=4, w=12, h=22, first=24, base=18, cap=12, xh=9, face=MONO["bold"], mono=10, sym=12),
    dict(set=0, font=5, w=8, h=16, first=17, base=13, cap=9, xh=7, face=MONO["medium"], mono=7, sym=8),
    # set 1 "New": proportional
    dict(set=1, font=1, w=14, h=20, first=32, base=17, cap=10, xh=7, face=SANS["light"], mono=0),
    dict(set=1, font=2, w=16, h=24, first=32, base=20, cap=12, xh=9, face=SANS["regular"], mono=0),
    dict(set=1, font=3, w=14, h=20, first=32, base=17, cap=10, xh=7, face=SANS["medium"], mono=0),
    dict(set=1, font=4, w=18, h=24, first=24, base=20, cap=12, xh=9, face=SANS["bold"], mono=0, sym=18),
    dict(set=1, font=5, w=12, h=17, first=17, base=14, cap=9, xh=6, face=SANS["light"], mono=0, sym=7),
]


def advances(spec):
    t = set0_table(spec["font"]) if spec["set"] == 0 else parse_table(SET1[spec["font"]])
    adv = [0] * 128
    for c, n in t.items():
        adv[c] = n
    if spec.get("sym"):
        for c in range(spec["first"], 32):
            adv[c] = spec["sym"]
    return adv


def cap_ratio(path):
    f = ImageFont.truetype(path, 1000)
    b = f.getbbox("H", anchor="ls")
    return (b[3] - b[1]) / 1000.0


def quantise(img):
    """8-bit coverage -> 4-bit nibbles (alpha = nibble * 17 on the display)."""
    return [min(15, (p * 15 + 127) // 255) for p in img.tobytes()]


def render_glyph(font_hr, ch, spec, sx, ref=None):
    """One glyph with its baseline on row spec['base'].

    Proportional fonts: ink left-aligned in the cell (lsb 0, spec 20 §2.1).
    Monospace fonts: the font's own placement, shifted so that the ink of
    "H" (`ref`, raw x of its left ink edge) starts at column 0; narrow glyphs
    (":", ".", "1") stay centred in the advance, no ink left of column 0 or
    right of the cell. spec["centre"] (set 0 fonts 1 / 3: 10-px cell, 8-px
    pen): the font's own placement with its advance box centred in the cell,
    i.e. 1 px right of the pen, as measured on the stock bar labels
    (Snapshot_010 "Play" ink 36..65, "Undo" 236..265 for pens 35 / 234)."""
    w, h, base = spec["w"], spec["h"], spec["base"]
    pad = 4 * K
    span = w * K / sx
    raw = Image.new("L", (int(span) + 2 * pad, h * K), 0)
    ImageDraw.Draw(raw).text((pad, base * K), ch, font=font_hr, fill=255, anchor="ls")
    ink = raw.getbbox()
    img = Image.new("L", (int(span) + K, h * K), 0)
    ink_w = 0.0
    if ink and spec.get("centre"):
        off = (w - spec["mono"]) / 2.0                 # output px right of the pen
        start = int(round(pad - off * K / sx))
        img.paste(raw.crop((start, 0, raw.size[0], h * K)), (0, 0))
        ink_w = (ink[2] - ink[0]) * sx / K
    elif ink:
        start = ink[0]
        if ref is not None:
            start = min(ink[0], ref)
            if ink[2] - start > span:
                start = max(ink[0], int(ink[2] - span))
        img.paste(raw.crop((start, 0, raw.size[0], h * K)), (0, 0))
        ink_w = (ink[2] - ink[0]) * sx / K
    small = img.resize((w, h), Image.BOX, box=(0, 0, span, h * K))
    return small, ink_w


def ink_left(font_hr, ch, spec):
    pad = 4 * K
    raw = Image.new("L", (spec["w"] * 2 * K + 2 * pad, spec["h"] * K), 0)
    ImageDraw.Draw(raw).text((pad, spec["base"] * K), ch, font=font_hr, fill=255, anchor="ls")
    return raw.getbbox()[0]


# --- our own symbol drawings (spec 20 §2.4: draw equivalents) --------------
def draw_pairs(spec):
    """Font 4 codes 24..31: four transport icons, each split over two cells."""
    adv, h, base, cap = spec["sym"], spec["h"], spec["base"], spec["cap"]
    W = 2 * adv * K
    top, bot = (base - cap) * K, base * K
    cells = []
    for icon in ("pause", "play", "playpat", "rec"):
        img = Image.new("L", (W, h * K), 0)
        d = ImageDraw.Draw(img)
        cx = W / 2
        ch = bot - top
        if icon == "pause":
            bw = ch * 0.28
            gap = ch * 0.26
            d.rectangle([cx - gap / 2 - bw, top, cx - gap / 2 - 1, bot - 1], fill=255)
            d.rectangle([cx + gap / 2, top, cx + gap / 2 + bw - 1, bot - 1], fill=255)
        elif icon == "play":
            tw = ch * 0.9
            d.polygon([(cx - tw / 2, top), (cx + tw / 2, (top + bot) / 2), (cx - tw / 2, bot)], fill=255)
        elif icon == "playpat":
            tw = ch * 0.8
            x0 = cx - tw / 2 - ch * 0.15
            d.rectangle([x0 - ch * 0.3, top, x0 - ch * 0.12, bot - 1], fill=255)
            d.polygon([(x0, top), (x0 + tw, (top + bot) / 2), (x0, bot)], fill=255)
        else:
            r = ch * 0.48
            cy = (top + bot) / 2
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        for half in (0, 1):
            part = img.crop((half * adv * K, 0, (half + 1) * adv * K, h * K))
            cell = Image.new("L", (spec["w"] * K, h * K), 0)
            cell.paste(part, (0, 0))
            cells.append(cell.resize((spec["w"], h), Image.BOX))
    return cells


SMALL_SYMBOLS = ["up", "down", "left", "right", "dot", "ring", "square", "box", "check", "cross",
                 "note", "loop", "plus", "minus", "ellipsis"]   # codes 17..31


def draw_small(spec, name):
    """Font 5 codes 17..31: small in-text symbols in the cap-height band."""
    adv, h, base, cap = spec["sym"], spec["h"], spec["base"], spec["cap"]
    img = Image.new("L", (spec["w"] * K, h * K), 0)
    d = ImageDraw.Draw(img)
    x0, x1 = 0.5 * K, (adv - 1.5) * K
    top, bot = (base - cap) * K, base * K
    cx, cy = (x0 + x1) / 2, (top + bot) / 2
    s = min(x1 - x0, bot - top)
    l, r, t, b = cx - s / 2, cx + s / 2, cy - s / 2, cy + s / 2
    lw = max(K, int(s * 0.16))
    if name == "up":
        d.polygon([(l, b - s * 0.2), (cx, t + s * 0.1), (r, b - s * 0.2)], fill=255)
    elif name == "down":
        d.polygon([(l, t + s * 0.2), (cx, b - s * 0.1), (r, t + s * 0.2)], fill=255)
    elif name == "left":
        d.polygon([(r - s * 0.15, t), (l + s * 0.1, cy), (r - s * 0.15, b)], fill=255)
    elif name == "right":
        d.polygon([(l + s * 0.15, t), (r - s * 0.1, cy), (l + s * 0.15, b)], fill=255)
    elif name == "dot":
        d.ellipse([l + s * 0.15, t + s * 0.15, r - s * 0.15, b - s * 0.15], fill=255)
    elif name == "ring":
        d.ellipse([l + s * 0.1, t + s * 0.1, r - s * 0.1, b - s * 0.1], outline=255, width=lw)
    elif name == "square":
        d.rectangle([l + s * 0.15, t + s * 0.15, r - s * 0.15, b - s * 0.15], fill=255)
    elif name == "box":
        d.rectangle([l + s * 0.1, t + s * 0.1, r - s * 0.1, b - s * 0.1], outline=255, width=lw)
    elif name == "check":
        d.line([(l, cy), (l + s * 0.38, b - s * 0.1), (r, t + s * 0.1)], fill=255, width=lw)
    elif name == "cross":
        d.line([(l + s * 0.1, t + s * 0.1), (r - s * 0.1, b - s * 0.1)], fill=255, width=lw)
        d.line([(r - s * 0.1, t + s * 0.1), (l + s * 0.1, b - s * 0.1)], fill=255, width=lw)
    elif name == "note":
        d.ellipse([l, b - s * 0.38, l + s * 0.5, b], fill=255)
        d.rectangle([l + s * 0.38, t, l + s * 0.38 + lw, b - s * 0.18], fill=255)
        d.line([(l + s * 0.38, t + lw / 2), (r, t + s * 0.3)], fill=255, width=lw)
    elif name == "loop":
        d.arc([l, t, r, b], 40, 320, fill=255, width=lw)
        d.polygon([(r - s * 0.05, cy - s * 0.45), (r + s * 0.05, cy + s * 0.05), (r - s * 0.45, cy - s * 0.1)], fill=255)
    elif name == "plus":
        d.rectangle([cx - lw / 2, t + s * 0.1, cx + lw / 2, b - s * 0.1], fill=255)
        d.rectangle([l + s * 0.1, cy - lw / 2, r - s * 0.1, cy + lw / 2], fill=255)
    elif name == "minus":
        d.rectangle([l + s * 0.1, cy - lw / 2, r - s * 0.1, cy + lw / 2], fill=255)
    elif name == "ellipsis":
        for i in range(3):
            px = l + s * (0.1 + 0.4 * i)
            d.ellipse([px, b - s * 0.25, px + s * 0.2, b - s * 0.05], fill=255)
    return img.resize((spec["w"], h), Image.BOX)


# --- icon sheet: IconId order of src/core/ui/gfx.h, 12x12 cells -------------
ICON_SIZE = 12
ICONS = ["play", "stop", "pause", "rec", "loop", "up", "down", "left", "right", "battery", "folder"]


def draw_icon(name):
    S = ICON_SIZE * K
    img = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(img)
    u = K   # one display pixel
    if name == "play":
        d.polygon([(1 * u, 1 * u), (11 * u, 6 * u), (1 * u, 11 * u)], fill=255)
    elif name == "stop":
        d.rectangle([1 * u, 1 * u, 11 * u - 1, 11 * u - 1], fill=255)
    elif name == "pause":
        d.rectangle([2 * u, 1 * u, 5 * u - 1, 11 * u - 1], fill=255)
        d.rectangle([7 * u, 1 * u, 10 * u - 1, 11 * u - 1], fill=255)
    elif name == "rec":
        d.ellipse([1 * u, 1 * u, 11 * u, 11 * u], fill=255)
    elif name == "loop":
        d.arc([1 * u, 2 * u, 11 * u, 11 * u], 300, 240, fill=255, width=int(1.5 * u))
        d.polygon([(4 * u, 0), (7.5 * u, 2.4 * u), (4 * u, 4.8 * u)], fill=255)
    elif name == "up":
        d.polygon([(2 * u, 8 * u), (6 * u, 4 * u), (10 * u, 8 * u)], fill=255)
    elif name == "down":
        d.polygon([(2 * u, 4 * u), (6 * u, 8 * u), (10 * u, 4 * u)], fill=255)
    elif name == "left":
        d.polygon([(8 * u, 2 * u), (4 * u, 6 * u), (8 * u, 10 * u)], fill=255)
    elif name == "right":
        d.polygon([(4 * u, 2 * u), (8 * u, 6 * u), (4 * u, 10 * u)], fill=255)
    elif name == "battery":
        d.rectangle([0, 3 * u, 10 * u - 1, 9 * u - 1], outline=255, width=u)
        d.rectangle([10 * u, 5 * u, 12 * u - 1, 7 * u - 1], fill=255)
        d.rectangle([2 * u, 5 * u, 6 * u - 1, 7 * u - 1], fill=255)
    elif name == "folder":
        d.rectangle([0, 2 * u, 5 * u - 1, 4 * u - 1], fill=255)
        d.rectangle([0, 3 * u, 12 * u - 1, 11 * u - 1], fill=255)
    return img.resize((ICON_SIZE, ICON_SIZE), Image.BOX)


def pack_l4(nibbles, w, h, stride):
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            v = nibbles[y * w + x]
            out[y * stride + x // 2] |= v << 4 if x % 2 == 0 else v   # FT81x L4: high nibble = left pixel
    return out


def align4(b):
    while len(b) % 4:
        b.append(0)


def build_ui(out_dir, preview=None):
    blob = bytearray()
    infos = []
    log = []
    sheets = []
    for spec in FONTS:
        path = os.path.join(FONTDIR, spec["face"])
        adv = advances(spec)
        size = spec["cap"] / cap_ratio(path)
        font_hr = ImageFont.truetype(path, size * K)
        if spec["mono"]:
            sx = spec["mono"] / (font_hr.getlength("0") / K)
        else:
            sx = 1.0   # proportional: natural widths, forced pen advance
        w, h, first = spec["w"], spec["h"], spec["first"]
        stride = (w + 1) // 2
        cells = []
        sym_pairs = draw_pairs(spec) if spec["font"] == 4 else None
        too_wide = []
        ref = ink_left(font_hr, "H", spec) if spec["mono"] else None
        for c in range(first, 127):
            if c < 32:
                img = sym_pairs[c - 24] if sym_pairs else draw_small(spec, SMALL_SYMBOLS[c - 17])
            else:
                img, ink_w = render_glyph(font_hr, chr(c), spec, sx, ref)
                if ink_w > w + 0.25:
                    too_wide.append(chr(c))
            cells.append(img)
        metric_off = len(blob)
        glyph_off = metric_off + 148
        char0_addr = RAMG_BASE + glyph_off - first * stride * h
        block = bytearray(adv)
        for v in (L4, stride, w, h, char0_addr):
            block += v.to_bytes(4, "little")
        blob += block
        for img in cells:
            blob += pack_l4(quantise(img), w, h, stride)
        align4(blob)
        infos.append((spec, metric_off, glyph_off, stride))
        sheets.append((spec, cells))
        log.append("set %d font %d: %s size %.2f px, x-scale %.3f, %d B%s" % (
            spec["set"], spec["font"], spec["face"], size, sx, len(blob) - metric_off,
            (", ink wider than cell: " + "".join(too_wide)) if too_wide else ""))
    icon_off = len(blob)
    icon_stride = ICON_SIZE // 2
    icon_imgs = [draw_icon(n) for n in ICONS]
    for img in icon_imgs:
        blob += pack_l4(quantise(img), ICON_SIZE, ICON_SIZE, icon_stride)
    align4(blob)
    if RAMG_BASE + len(blob) > RAMG_END:
        raise SystemExit("font image %d B does not fit the RAM_G band" % len(blob))
    crc = zlib.crc32(bytes(blob)) & 0xFFFFFFFF

    import PIL
    head = ["// GENERATED by tools/fonts/gen_fonts.py (make fonts, Pillow %s) -- do not edit." % PIL.__version__,
            "// UI fonts of spec 20 §2 rendered from Roboto Mono (OFL-1.1) and Roboto",
            "// (Apache-2.0), third_party/fonts; symbols and icons are our own drawings."]
    o = list(head)
    for line in log:
        o.append("//   " + line)
    o.append("// RAM_G image: per font a 148-byte legacy metric block (CMD_SETFONT) +")
    o.append("// L4 cells from the first char; then %d icon cells %dx%d L4 (kBlob: font_data.inc)." % (
        len(ICONS), ICON_SIZE, ICON_SIZE))
    o.append("#pragma once")
    o.append("constexpr uint32_t kRamGBase = 0x%06X;" % RAMG_BASE)
    o.append("constexpr uint32_t kBlobSize = %d;" % len(blob))
    o.append("constexpr uint32_t kBlobCrc = 0x%08X;   // CRC-32 (zlib) of kBlob" % crc)
    o.append("// {metric offset, glyph offset, first char, cell w, cell h, stride, baseline, cap height}")
    o.append("constexpr FontInfo kFonts[2][5] = {")
    for s in range(2):
        rows = []
        for spec, mo, go, st in infos:
            if spec["set"] == s:
                rows.append("{%d, %d, %d, %d, %d, %d, %d, %d}" % (mo, go, spec["first"], spec["w"], spec["h"], st,
                                                                  spec["base"], spec["cap"]))
        o.append("    {" + ", ".join(rows) + "},")
    o.append("};")
    o.append("constexpr IconSheet kIcons = {%d, %d, %d, %d, %d};   // offset, w, h, stride, count" % (
        icon_off, ICON_SIZE, ICON_SIZE, icon_stride, len(ICONS)))
    with open(os.path.join(out_dir, "font_data.h"), "w") as f:
        f.write("\n".join(o) + "\n")
    o = list(head)
    o.append("// The RAM_G image (included once, by src/core/ui/fonts/fonts.cpp).")
    for i in range(0, len(blob), 40):
        o.append(",".join(str(b) for b in blob[i:i + 40]) + ",")
    with open(os.path.join(out_dir, "font_data.inc"), "w") as f:
        f.write("\n".join(o) + "\n")
    for line in log:
        print(line)
    print("blob %d B (0x%06X..0x%06X), crc 0x%08X" % (len(blob), RAMG_BASE, RAMG_BASE + len(blob) - 1, crc))

    if preview:
        rows = []
        for spec, cells in sheets:
            adv = advances(spec)
            first = spec["first"]
            line = "".join(chr(c) for c in range(first, 127))
            width = sum(adv[ord(c)] for c in line) + 4
            img = Image.new("L", (width, spec["h"]), 0)
            x = 2
            for ch, cell in zip(line, cells):
                q = Image.new("L", cell.size)
                q.frombytes(bytes(n * 17 for n in quantise(cell)))
                img.paste(q, (x, 0), q)
                x += adv[ord(ch)]
            rows.append(img)
        icons = Image.new("L", (len(ICONS) * 16, 16), 0)
        for i, im in enumerate(icon_imgs):
            q = Image.new("L", im.size)
            q.frombytes(bytes(n * 17 for n in quantise(im)))
            icons.paste(q, (i * 16 + 2, 2))
        rows.append(icons)
        W = max(r.size[0] for r in rows)
        H = sum(r.size[1] + 2 for r in rows)
        sheet = Image.new("L", (W, H), 0)
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.size[1] + 2
        sheet.resize((W * 3, H * 3), Image.NEAREST).save(preview)


# ---------------------------------------------------------------------------
# FT81x ROM font look-alikes for the simulator (unchanged from sim/tools).
def build_rom(sans, mono, out_path):
    # handle: (cell height, fixed width or 0, antialiased, mono face, cap-ish size)
    fonts = {
        16: (8, 8, False, True, 8), 17: (8, 8, False, True, 8),
        18: (16, 8, False, True, 13), 19: (16, 8, False, True, 13),
        20: (13, 0, False, False, 10), 21: (17, 0, False, False, 13),
        22: (20, 0, False, False, 15), 23: (22, 0, False, False, 17),
        24: (29, 0, False, False, 22), 25: (38, 0, False, False, 29),
        26: (16, 0, True, False, 12), 27: (20, 0, True, False, 15),
        28: (25, 0, True, False, 19), 29: (28, 0, True, False, 22),
        30: (36, 0, True, False, 28), 31: (49, 0, True, False, 38),
    }
    out = open(out_path, "w")
    out.write("// Generated by tools/fonts/gen_fonts.py rom from DejaVu Sans / Sans Mono.\n")
    out.write("// Bitstream Vera licence: sim/third_party/DEJAVU_LICENSE.txt\n")
    out.write("// Approximations of the FT81x ROM fonts 16..31 (chars 32..126), 8-bit alpha.\n")
    tables = []
    for handle, (h, fixed, aa, is_mono, size) in fonts.items():
        font = ImageFont.truetype(mono if is_mono else sans, size)
        asc, desc = font.getmetrics()
        base = min(asc, h - 1) if asc + desc > h else asc + (h - asc - desc) // 2
        widths, offs, data = [], [], bytearray()
        for c in range(32, 127):
            ch = chr(c)
            adv = fixed if fixed else max(1, int(round(font.getlength(ch))))
            img = Image.new("L", (adv, h), 0)
            d = ImageDraw.Draw(img)
            if not aa:
                d.fontmode = "1"
            x0 = (fixed - font.getlength(ch)) / 2 if fixed else 0
            d.text((x0, base), ch, font=font, fill=255, anchor="ls")
            px = img.tobytes()
            if not aa:
                px = bytes(255 if p >= 128 else 0 for p in px)
            offs.append(len(data))
            widths.append(adv)
            data += px
        name = "font%d" % handle
        out.write("static const uint8_t %s_px[%d] = {\n" % (name, len(data)))
        for i in range(0, len(data), 32):
            out.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
        out.write("};\n")
        out.write("static const uint8_t %s_w[95] = {%s};\n" % (name, ",".join(map(str, widths))))
        out.write("static const uint32_t %s_off[95] = {%s};\n" % (name, ",".join(map(str, offs))))
        tables.append("{%d, %d, %s_w, %s_off, %s_px}" % (h, base, name, name, name))
    out.write("static const RomFont kRomFonts[16] = {\n  " + ",\n  ".join(tables) + "\n};\n")
    out.close()


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "ui":
        pv = a[a.index("--preview") + 1] if "--preview" in a else None
        build_ui(a[1], pv)
    elif len(a) == 4 and a[0] == "rom":
        build_rom(a[1], a[2], a[3])
    else:
        sys.exit(__doc__)
