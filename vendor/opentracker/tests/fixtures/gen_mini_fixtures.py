# Generates the Tracker Mini fixtures used by tests/pattern_mtp_test.cpp and
# tests/mini_fixture_test.cpp, from docs/specs/21-trackerplus-mini-formats.md
# and the MIT tracker-lib layout constants, without using the C++ codecs.
#
# Usage: python3 -I gen_mini_fixtures.py <fixtures dir>
#
# Writes:
#   mini_pattern_golden.hex        0x3030 pattern, content = goldenStep() below
#   mini/<project>/project.mt      Mini projects built on tl_project_template.mt
#   mini/<project>/patterns/pattern_NN.mtp
import os, struct, sys, zlib

# tracker-lib src/types/patterns.ts PatternConstants.
HEADER_SIZE, PADDING_SIZE, UNUSED_SIZE = 14, 2, 12
TRACK_COUNT_MINI_PLUS, STEP_COUNT, STEP_SIZE, TRACK_HEADER_SIZE, CRC_SIZE = 16, 128, 6, 1, 4
TRACK_SIZE = TRACK_HEADER_SIZE + STEP_SIZE * STEP_COUNT
TRACKS_OFF = HEADER_SIZE + PADDING_SIZE + UNUSED_SIZE
MTP_SIZE = TRACKS_OFF + TRACK_SIZE * TRACK_COUNT_MINI_PLUS + CRC_SIZE
assert (TRACKS_OFF, TRACK_SIZE, MTP_SIZE) == (0x1C, 769, 0x3030)   # spec 21 §4.1

def crc(d): return zlib.crc32(d) & 0xffffffff   # CRC-32/ISO-HDLC (spec 21 U1)

# Step content: (note, instrument, fx1 type, fx1 value, fx0 type, fx0 value);
# None = empty step (note 0xFF, other bytes 0). Instruments cycle 0..66 so
# synth slots 64..66 appear on every track (spec 21 §4.1).
def goldenStep(seed, t, s):
    if (s + t + seed) % 5:
        return None
    k = t * 131 + s * 7 + seed * 17
    return ((24 + k) % 128, (t * 3 + s + seed) % 67, k % 43, (k * 5) & 0xff, (k // 3) % 43, (k * 11) & 0xff)

def goldenLastStep(seed, t): return (t * 7 + 15 + seed) % 128

def mtp(seed):
    hdr = b'KS' + struct.pack('<H', 2) + bytes([1, 9, 2, 1]) + bytes([5] * 4) + struct.pack('<H', MTP_SIZE)
    d = bytearray(hdr + bytes(PADDING_SIZE + UNUSED_SIZE))
    for t in range(TRACK_COUNT_MINI_PLUS):
        d.append(goldenLastStep(seed, t))
        for s in range(STEP_COUNT):
            st = goldenStep(seed, t, s)
            d += bytes([0xFF, 0, 0, 0, 0, 0]) if st is None else bytes(st)
    d += struct.pack('<I', crc(bytes(d)))
    assert len(d) == MTP_SIZE
    return bytes(d)

def dump(d, title):
    out = [f"# {title}", f"# {len(d)} bytes, generated independently of the C++ codec by gen_mini_fixtures.py (python zlib.crc32)."]
    for o in range(0, len(d), 16):
        out.append(f"{o:04x}: " + ' '.join(f'{b:02x}' for b in d[o:o + 16]))
    return '\n'.join(out) + '\n'

# --- project.mt (spec 21 §3) --------------------------------------------------
def slot(gain, poly, model, pan, name, rev, dly, macros):
    n = name.encode('ascii')
    assert len(n) < 32 and len(macros) == 6
    r = struct.pack('<fIIf', gain, poly, model, pan) + n + bytes(32 - len(n)) + struct.pack('<ff', rev, dly) + bytes(macros) + bytes(2)
    assert len(r) == 0x40
    return r

def project(tpl, name, slots, names13, mutes13):
    d = bytearray(tpl)
    assert len(d) == 0x914 and d[:2] == b'MT' and d[8] == 17
    d[0x0E:0x10] = bytes(2)                      # header padding (writers put 0)
    for s, r in enumerate(slots):                # synth slots at 0x74C + 0x40*s
        d[0x74C + 0x40 * s:0x74C + 0x40 * (s + 1)] = r
    for i, n in enumerate(names13):              # names 13..16: 0x603 slots 4..7
        b = n.encode('ascii')
        assert len(b) < 8
        o = 0x603 + 8 * (4 + i)
        d[o:o + 8] = b + bytes(8 - len(b))
    for i, m in enumerate(mutes13):              # mutes 13..16: 0x742[4..7] (U13)
        d[0x742 + 4 + i] = m
    nm = name.encode('ascii')
    d[0x810:0x831] = nm + bytes(0x21 - len(nm))  # name char[32] + forced NUL
    fo = b'Projects'
    d[0x831:0x910] = fo + bytes(0x910 - 0x831 - len(fo))
    d[0x910:0x914] = struct.pack('<I', crc(bytes(d[:0x910])))
    return bytes(d)

PROJECTS = [
    ("Mini Synth Song",
     [slot(-3.5, 3, 4, -25.0, "Kit 808", 12.5, 0.0, [0, 10, 63, 127, 5, 0]),     # PERC
      slot(0.0, 2, 0, 40.0, "Acid Line", 0.0, 30.0, [64, 0, 0, 0, 0, 1]),        # ACD
      slot(-96.0, 3, 2, 100.0, "Pad Wide", 100.0, 100.0, [63] * 6)],             # VAP
     ["Bass", "Lead", "Perc", "Pad"], [0, 1, 0, 1], [1, 2]),
    ("Mini Edge Slots",
     [slot(12.0, 8, 3, -100.0, "", 0.0, 0.0, [127] * 6),                         # WTFM, range ends
      slot(-9.0, 0, 1, 0.0, "Fat Saw Bass 0123456789abcdefgh", 50.0, 25.0, [0] * 6),   # FAT, poly 0
      slot(-9.0, 1, 2, 0.0, "Init", 0.0, 0.0, [0] * 6)],
     ["Midi 13", "x", "", "Seven77"], [1, 1, 1, 1], [3]),
]

def main(out):
    open(os.path.join(out, 'mini_pattern_golden.hex'), 'w').write(
        dump(mtp(0), "pattern golden, Tracker Mini 16 tracks (spec 21 §4.1): track t last step=(7t+15)%128; "
                     "step s non-empty when (s+t)%5==0 (see goldenStep in gen_mini_fixtures.py)"))
    tpl = open(os.path.join(out, 'tl_project_template.mt'), 'rb').read()
    for name, slots, names13, mutes13, pats in PROJECTS:
        base = os.path.join(out, 'mini', name)
        os.makedirs(os.path.join(base, 'patterns'), exist_ok=True)
        open(os.path.join(base, 'project.mt'), 'wb').write(project(tpl, name, slots, names13, mutes13))
        for n in pats:
            open(os.path.join(base, 'patterns', f'pattern_{n:02d}.mtp'), 'wb').write(mtp(n))
    print("ok", MTP_SIZE)

main(sys.argv[1])
