// Legacy format upgrades, spec 10.
#include "formats/legacy_upgrade.h"

#include <cmath>
#include <cstring>

#include "formats/bytes.h"
#include "formats/instrument_pti.h"
#include "model/fx.h"

namespace ot {
namespace legacy {

// ---------------------------------------------------------------------------
// spec 10 §0.4 gain curves. Every operation is one single-precision rounding,
// in exactly the stated order. `volatile` keeps the compiler from contracting
// or reassociating (the device build uses GNU mode, where contraction is on).
namespace {
uint8_t gainCurve(uint8_t v, float a, float a0, float b0, float b) {
    if (v == 0) return 0;
    volatile float x = float(v) - 1.0f;
    x = x * a;
    x = x / 99.0f;
    x = x - a0;
    x = x + b0;
    x = x * 99.0f;
    x = x / b;
    x = x + 1.0f;
    float r = x;
    if (r > 0.0f) {
        x = r + 0.5f;
        r = x;
    }
    if (r < 1.0f) return 1;
    if (r > 100.0f) return 100;
    return uint8_t(unsigned(r));
}
}  // namespace

uint8_t gainG1(uint8_t v) { return gainCurve(v, 19.8f, 19.8f, 36.0f, 42.0f); }
uint8_t gainG2(uint8_t v) { return gainCurve(v, 19.8f, 19.8f, 24.0f, 48.0f); }
uint8_t gainG3(uint8_t v) { return gainCurve(v, 42.0f, 36.0f, 18.0f, 21.0f); }
uint8_t gainG4(uint8_t v) { return gainCurve(v, 48.0f, 24.0f, 12.0f, 24.0f); }

uint16_t logLG(uint16_t v) {
    if (v == 0) return 0;
    volatile float q = float(v) / 32767.0f;
    volatile float y = log10f(q);
    volatile float z = fmaf(y, 10.0f, 36.3f);   // fused, spec 10 §0.4
    z = z * 32766.0f;
    z = z / 36.3f;
    z = z + 1.0f;
    float r = z;
    if (r >= 0.0f) z = r + 0.5f;
    else z = r - 0.5f;
    r = z;
    if (r <= 0.0f) return 0;            // negative saturates to 0
    if (r >= 65535.0f) return 65535;    // SPEC GAP: cannot happen for u16 input
    return uint16_t(unsigned(r));
}

// ---------------------------------------------------------------------------
// spec 10 §1.2 project.mt chain (file offsets).
namespace {
void applyGain(uint8_t* img, uint8_t (*g)(uint8_t), const uint32_t* offs, size_t n) {
    for (size_t i = 0; i < n; ++i) img[offs[i]] = g(img[offs[i]]);
}
void putCStr(uint8_t* p, const char* s) {
    size_t n = std::strlen(s);
    std::memcpy(p, s, n + 1);   // string + NUL; rest of the slot untouched
}
}  // namespace

void projectStep(uint8_t* img, uint8_t from, uint32_t& c) {
    static const uint32_t kLevelsG1[] = {0x4D0, 0x4D1, 0x4D2, 0x4D3, 0x4D4, 0x4D5,
                                         0x4D6, 0x4D7, 0x4DD, 0x4D8, 0x4D9};
    static const uint32_t kLevelsG3[] = {0x4D0, 0x4D1, 0x4D2, 0x4D3, 0x4D4, 0x4D5, 0x4D6,
                                         0x4D7, 0x4DD, 0x4D8, 0x4D9, 0x309};
    switch (from) {
    case 1: {   // insert 4 bytes at 0x11C (delay feedback/time)
        // 0x11C..0x80A -> 0x120..0x80E, copied from a snapshot (= memmove).
        std::memmove(img + 0x120, img + 0x11C, 0x80A - 0x11C + 1);
        img[0x11A] = 50;
        wrU16(img + 0x11C, 500);
        img[0x11E] = 0;   // 0x11F keeps its pre-step value
        c += 4;
        break;
    }
    case 2:     // reverb parameters
        wrF32(img + 0x418, 0.5f);
        wrF32(img + 0x41C, 0.5f);
        wrF32(img + 0x420, 0.5f);
        wrU32(img + 0x424, 0x3F2E147Bu);
        c += 16;
        break;
    case 3:     // track names 1-8
        for (int i = 1; i <= 8; ++i) {
            char s[12] = {'T', 'r', 'a', 'c', 'k', ' ', char('0' + i), 0};
            putCStr(img + 0x428 + 21 * (i - 1), s);
        }
        c += 168;
        break;
    case 4:
        std::memset(img + 0x4D0, 100, 10);
        c += 10;
        break;
    case 5:
        wrU16(img + 0x4DA, 0);
        img[0x4DC] = 0;
        img[0x4DD] = 100;
        c += 4;
        break;
    case 6:
        applyGain(img, gainG1, kLevelsG1, sizeof(kLevelsG1) / sizeof(kLevelsG1[0]));
        break;
    case 7:
        wrU16(img + 0x128, logLG(rdU16(img + 0x128)));
        std::memset(img + 0x4E0, 0, 22);
        c += 22;
        break;
    case 8:
        img[0x30A] = 1;
        img[0x309] = 86;
        c += 2;
        break;
    case 9:
        img[0x115] = 0;
        img[0x116] = 11;
        img[0x117] = 24;
        wrU32(img + 0x4F8, 0x00000103u);
        img[0x4FC] = 1;
        c += 5;
        break;
    case 10: {  // 16 MIDI-instrument records
        for (int i = 0; i < 16; ++i) {
            uint8_t* r = img + 0x4FD + 16 * i;
            const uint8_t rec[8] = {uint8_t(i), 127, 1, 5, 7, 11, 64, 128};
            std::memcpy(r, rec, 8);
            std::memset(r + 8, 0, 8);
            r[1] = img[0x309 + i];
        }
        c += 16;
        break;
    }
    case 11:
        applyGain(img, gainG3, kLevelsG3, sizeof(kLevelsG3) / sizeof(kLevelsG3[0]));
        break;
    case 12: {
        if (img[0x30B] > 100) {
            img[0x30B] = 0;
            img[0x30C] = 0;
        }
        if (img[0x30D] > 100) {
            img[0x30D] = 0;
            img[0x30E] = 0;
        }
        const uint8_t o6 = img[0x4F6], o7 = img[0x4F7];
        img[0x30A] = o6;
        img[0x309] = o7;
        wrU16(img + 0x4F6, 0);
        break;
    }
    case 13:
        wrU16(img + 0x5FE, 20000);
        c += 2;
        break;
    case 14:
        wrU16(img + 0x600, 0);
        img[0x602] = 0;
        c += 3;
        break;
    case 15: {
        // (c) first, so it reads pre-step bytes (the snapshot): C string
        // from 0x609 - C to 0x810. Dead for legacy files (size field !=
        // 0x914, spec 10 §1.2 notes); reproduced. SPEC GAP: the firmware
        // strcpy is unbounded; OpenTracker stops at the image end or when
        // the source reaches the destination.
        if (c <= 0x609u) {
            for (uint32_t src = 0x609u - c, d = 0x810; d < kProjectImageSize && src < 0x810; ++d, ++src) {
                img[d] = img[src];
                if (!img[src]) break;
            }
        }
        // (a) "Midi 9".."Midi 16" at 0x603 + 8k.
        for (int t = 8; t < 16; ++t) {
            char s[10] = {'M', 'i', 'd', 'i', ' ', 0, 0, 0, 0, 0};
            int n = t + 1;
            if (n >= 10) {
                s[5] = char('0' + n / 10);
                s[6] = char('0' + n % 10);
            } else {
                s[5] = char('0' + n);
            }
            putCStr(img + 0x603 + 8 * (t - 8), s);
        }
        // (b) zero 0x643..0x749.
        std::memset(img + 0x643, 0, 0x749 - 0x643 + 1);
        c += 0x207;
        break;
    }
    case 16:
        img[0x80C] = 0;
        break;
    default:
        break;
    }
}

bool upgradeProjectImage(uint8_t* img, uint8_t fromVersion) {
    if (fromVersion == 0) return false;   // spec 10 §0.3: reject
    uint32_t c = 0;
    for (uint8_t v = fromVersion; v < kProjectTargetVersion; ++v) projectStep(img, v, c);
    return true;
}

// ---------------------------------------------------------------------------
// spec 10 §2.2 pattern steps. Scan rule: tracks 0..7; skip lastStep 0xFF;
// steps 0..lastStep; sample instruments only.
namespace {
template <typename F>
void scanSteps(Track* tracks, F fn) {
    for (int t = 0; t < kLegacyTrackCount; ++t) {
        Track& tr = tracks[t];
        if (tr.lastStep == 0xFF) continue;
        // SPEC GAP: lastStep 128..254 would run past the track in the
        // firmware's RAM layout; OpenTracker stops at step 127.
        int last = tr.lastStep < kStepCount ? tr.lastStep : kStepCount - 1;
        for (int s = 0; s <= last; ++s) {
            Step& st = tr.steps[s];
            if (!isSampleInstrument(st.instrument)) continue;
            fn(st);
        }
    }
}
}  // namespace

void patternStepVolume(Track* p, uint8_t (*curve)(uint8_t)) {
    scanSteps(p, [&](Step& st) {
        for (auto& f : st.fx)
            if (f.type == uint8_t(FxId::Volume)) f.value = curve(f.value);
    });
}

void patternStepBeatSlice(Track* p, const uint8_t* modes, uint8_t ref117) {
    const int d = 12 - int(ref117);
    scanSteps(p, [&](Step& st) {
        if (st.note < 0) return;
        if (modes[st.instrument] != uint8_t(PlayMode::BeatSlice)) return;
        int n = st.note + d;
        st.note = int8_t(n < 0 ? 0 : (n > 120 ? 120 : n));
    });
}

void patternStepExpandTracks(Track* p, int trackCount) {
    for (int t = kLegacyTrackCount; t < trackCount; ++t) p[t].clear(p[0].lastStep);
}

bool upgradePattern(Track* p, uint8_t v, const PatternUpgradeParams& prm, int trackCount) {
    if (v >= 5) return false;
    if (v < 2) patternStepVolume(p, prm.curveV2 ? prm.curveV2 : gainG2);
    if (v < 3 && prm.instrumentPlayModes) patternStepBeatSlice(p, prm.instrumentPlayModes, prm.beatSliceReference);
    if (v < 4) patternStepVolume(p, prm.curveV4 ? prm.curveV4 : gainG4);
    patternStepExpandTracks(p, trackCount);
    return true;
}

// ---------------------------------------------------------------------------
// spec 10 §3.2 .pti chain (file offsets).
void ptiStep(uint8_t* h, uint8_t from) {
    switch (from) {
    case 1:
        if (h[0x4C] > 4) h[0x4C] = uint8_t(h[0x4C] + 1);
        break;
    case 2:
        wrF32(h + 0xC0, 1.0f);
        wrU16(h + 0xC4, 0);
        wrU16(h + 0xC6, 3000);
        wrU16(h + 0xC8, 0);
        wrU16(h + 0xCA, 0);
        wrF32(h + 0xCC, 1.0f);
        wrU16(h + 0xD0, 1000);
        h[0xD2] = 0;
        h[0xD3] = 0;
        break;
    case 3: h[0x180] = 0; break;
    case 4: h[0x110] = gainG2(h[0x110]); break;
    case 5: h[0x181] = 0; break;
    case 6: h[0x182] = 16; break;
    case 7: h[0x183] = 0; break;
    case 8: h[0x110] = gainG4(h[0x110]); break;
    default: break;
    }
}

void upgradePtiImage(uint8_t* hdr, uint8_t fromVersion) {
    if (fromVersion == 0) fromVersion = 1;   // spec 10 §0.3 recommendation
    for (uint8_t v = fromVersion; v < kPtiTargetVersion; ++v) ptiStep(hdr, v);
}

FmtStatus readLegacyPtiHeader(const uint8_t* file, size_t len, uint8_t hdr[kPtiHeaderImageSize],
                              uint8_t* structVersion, uint32_t* dataOffset) {
    std::memset(hdr, 0, kPtiHeaderImageSize);
    if (len < kCommonHeaderSize) return FmtStatus::TooShort;
    std::memcpy(hdr, file, len < kPtiHeaderImageSize ? len : kPtiHeaderImageSize);
    if (rdU16(hdr + 2) != 1) return FmtStatus::WrongType;
    const uint8_t v = hdr[8];
    if (v < kPtiTargetVersion) upgradePtiImage(hdr, v);
    if (structVersion) *structVersion = v;
    if (dataOffset) *dataOffset = uint32_t(rdU16(hdr + 0x0C)) + 0x14;
    return FmtStatus::Ok;
}

void applyImportedLength(Instrument& ins, uint32_t frames, bool stereo) {
    ins.lengthFrames = frames;
    ins.stereo = stereo;
    // spec 10 §3.3 step 5: grain length clamped to the length (I).
    if (ins.grainLength > frames) ins.grainLength = uint16_t(frames);
    // SPEC GAP: wavetable parameter refresh for play mode 6 not specified.
}

size_t buildPtiFromMti(const uint8_t* mti, size_t mtiLen, const uint8_t* wavFile, size_t wavLen,
                       int16_t* scratch, size_t scratchCap, uint8_t* out, size_t outCap, Instrument* insOut,
                       FmtStatus* status, bool stereo) {
    auto fail = [&](FmtStatus s) -> size_t {
        if (status) *status = s;
        return 0;
    };
    Instrument ins;
    PtiLoadInfo li;
    FmtStatus st = decodePtiHeader(mti, mtiLen, ins, &li);
    if (st != FmtStatus::Ok) return fail(st);
    if (!ptiSlotAccepted(mtiLen, li.dataOffset)) return fail(FmtStatus::TooShort);
    WavImportOptions opt;
    opt.stereo = stereo;
    WavImportResult res;
    st = wavImportMemory(wavFile, wavLen, opt, scratch, scratchCap, res);
    if (st != FmtStatus::Ok) return fail(st);
    applyImportedLength(ins, res.frames, res.stereo);
    const size_t n = encodePti(ins, scratch, res.stereo ? scratch + res.frames : nullptr, out, outCap);
    if (!n) return fail(FmtStatus::NoSpace);
    if (insOut) *insOut = ins;
    if (status) *status = FmtStatus::Ok;
    return n;
}

}  // namespace legacy
}  // namespace ot
