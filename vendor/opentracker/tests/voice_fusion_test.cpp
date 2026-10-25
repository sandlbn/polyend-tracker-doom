// OP-3 (docs/OPTIMIZATION_2X.md): the fused voice chain (player * envelope,
// crusher, overdrive, SVF, sends, AMP, pan, dry-bus sum in one pass; used when
// the engine does not need o.l / o.r) must be bit-identical to the general
// stage-by-stage path. Each case renders the same note twice -- keepLR = true
// (general path / fusedOut<kLR>) and keepLR = false (fused paths) -- and
// compares o.amp, both sends and the dry-bus sums bit for bit.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "test.h"

#include "audio/engine.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr uint32_t kLen = 30000;

std::vector<int16_t> source() {
    std::vector<int16_t> d(kLen);
    uint32_t s = 5;
    for (uint32_t i = 0; i < kLen; ++i) {
        s = s * 1664525u + 1013904223u;
        d[i] = int16_t(26000.0f * std::sin(float(i) * 0.013f) + float(int32_t(s) >> 20));
    }
    return d;
}

struct Run {
    std::vector<float> amp, ds, rs, ml, mr;
};

bool bitsEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

Run render(const Instrument& ins, const int16_t* data, bool keepLR, const VoiceControl& c, int blocks) {
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    v->noteOn(&ins, data, ins.lengthFrames, 57, -1, kFxGlide, 12, 0, 0);
    Run r;
    std::unique_ptr<VoiceOut> o(new VoiceOut());
    for (int b = 0; b < blocks; ++b) {
        float ds[kBlock], rs[kBlock], ml[kBlock], mr[kBlock];
        for (uint32_t i = 0; i < kBlock; ++i) {   // non-zero buses (other voices' sums)
            ds[i] = rs[i] = 0.01f * float(i & 7);
            ml[i] = mr[i] = -0.02f * float(i & 3);
        }
        if (b == 6) v->applyFx(0, kFxLowPass, 40);   // filter change: cross-fade block
        if (b == 9) v->applyFx(1, kFxBitDepth, 6);
        if (b == 14) v->applyFx(0, kFxHighPass, 70);
        if (b == 20) v->applyFx(1, kFxOverdrive, 80);
        if (b == 26) v->applyFx(0, kFxBandPass, 20);
        if (b == 30) v->noteOff(-4);
        v->control(c);
        v->render(c, *o, ds, rs, ml, mr, keepLR);
        r.amp.insert(r.amp.end(), o->amp, o->amp + kBlock);
        r.ds.insert(r.ds.end(), ds, ds + kBlock);
        r.rs.insert(r.rs.end(), rs, rs + kBlock);
        r.ml.insert(r.ml.end(), ml, ml + kBlock);
        r.mr.insert(r.mr.end(), mr, mr + kBlock);
    }
    return r;
}

}  // namespace

TEST(fused_voice_chain_bit_identical) {
    initTables();
    const auto data = source();
    int cases = 0, bad = 0;
    for (int mode = 0; mode < 8; ++mode)
        for (int filt = 0; filt < 4; ++filt)
            for (int fx = 0; fx < 4; ++fx)
                for (int sends = 0; sends < 4; ++sends) {
                    Instrument ins;
                    ins.lengthFrames = kLen;
                    ins.playMode = uint8_t(mode);
                    ins.wavetableWindowSize = 2048;
                    ins.wavetableWindowCount = 8;
                    ins.filterEnabled = filt != 0;
                    ins.filterType = uint8_t(filt == 0 ? 0 : filt - 1);
                    ins.cutoff = 0.25f + 0.15f * float(filt);
                    ins.resonance = 0.3f * float(fx);
                    ins.bitDepth = uint8_t(fx & 1 ? 9 : 16);
                    ins.overdrive = uint8_t(fx & 2 ? 35 + 10 * mode : 0);
                    ins.delaySend = uint8_t(sends & 1 ? 40 : 0);
                    ins.reverbSend = uint8_t(sends & 2 ? 70 : 0);
                    ins.panning = int16_t(10 + 10 * mode);
                    ins.volume = uint8_t(60 + 10 * fx);   // up to +AMP clamp territory
                    ins.env[kAutoCutoff].enabled = filt == 3;   // modulated cutoff
                    ins.env[kAutoCutoff].loop = 1;
                    ins.env[kAutoPanning].enabled = mode & 1;
                    ins.env[kAutoPanning].loop = 1;
                    VoiceControl c;
                    c.headroom = (mode + fx) & 1;
                    const Run a = render(ins, data.data(), true, c, 40);
                    const Run b = render(ins, data.data(), false, c, 40);
                    ++cases;
                    if (!bitsEqual(a.amp, b.amp) || !bitsEqual(a.ds, b.ds) || !bitsEqual(a.rs, b.rs) ||
                        !bitsEqual(a.ml, b.ml) || !bitsEqual(a.mr, b.mr)) {
                        if (bad < 5)
                            std::printf("    differ: mode %d filt %d fx %d sends %d\n", mode, filt, fx, sends);
                        ++bad;
                    }
                }
    std::printf("  %d voice configurations compared, %d differ\n", cases, bad);
    CHECK_EQ(bad, 0);
}

// Stereo notes: the filter-only fused path (dry-bus pointers given) against
// the general stereo path (no dry-bus pointers; the bus sum is o.l / o.r).
namespace {
Run renderSt(const Instrument& ins, const int16_t* l, const int16_t* r, bool fused, const VoiceControl& c,
             int blocks, std::vector<float>* ol, std::vector<float>* orr) {
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    v->noteOn(&ins, l, ins.lengthFrames, 62, -1, kFxGlide, 9, 0, 0, r);
    Run res;
    std::unique_ptr<VoiceOut> o(new VoiceOut());
    for (int b = 0; b < blocks; ++b) {
        float ds[kBlock], rs[kBlock], ml[kBlock], mr[kBlock];
        for (uint32_t i = 0; i < kBlock; ++i) {
            ds[i] = rs[i] = 0.01f * float(i & 7);
            ml[i] = mr[i] = -0.02f * float(i & 3);
        }
        if (b == 8) v->applyFx(0, kFxHighPass, 55);   // type change: cross-fade block
        if (b == 16) v->applyFx(0, kFxBandPass, 30);
        if (b == 24) v->noteOff(-4);
        v->control(c);
        v->render(c, *o, ds, rs, fused ? ml : nullptr, fused ? mr : nullptr, false);
        if (!fused)
            for (uint32_t i = 0; i < kBlock; ++i) {
                ml[i] += o->l[i];
                mr[i] += o->r[i];
            }
        res.amp.insert(res.amp.end(), o->amp, o->amp + kBlock);
        res.ds.insert(res.ds.end(), ds, ds + kBlock);
        res.rs.insert(res.rs.end(), rs, rs + kBlock);
        res.ml.insert(res.ml.end(), ml, ml + kBlock);
        res.mr.insert(res.mr.end(), mr, mr + kBlock);
        ol->insert(ol->end(), o->l, o->l + kBlock);
        orr->insert(orr->end(), o->r, o->r + kBlock);
    }
    return res;
}
}  // namespace

TEST(fused_stereo_filter_bit_identical) {
    initTables();
    const auto left = source();
    std::vector<int16_t> right(left.rbegin(), left.rend());
    int cases = 0, bad = 0;
    for (int mode = 0; mode <= int(PlayMode::BeatSlice); ++mode)
        for (int filt = 0; filt < 3; ++filt)
            for (int sends = 0; sends < 4; ++sends)
                for (int tap = 0; tap < 2; ++tap) {
                    Instrument ins;
                    ins.lengthFrames = kLen;
                    ins.playMode = uint8_t(mode);
                    ins.filterEnabled = 1;
                    ins.filterType = uint8_t(filt);
                    ins.cutoff = 0.3f + 0.2f * float(filt);
                    ins.resonance = 0.4f;
                    ins.delaySend = uint8_t(sends & 1 ? 45 : 0);
                    ins.reverbSend = uint8_t(sends & 2 ? 65 : 0);
                    ins.panning = int16_t(15 + 12 * mode);
                    ins.volume = uint8_t(70 + 10 * filt);
                    ins.env[kAutoCutoff].enabled = 1;
                    ins.env[kAutoCutoff].loop = 1;
                    ins.env[kAutoPanning].enabled = sends & 1;
                    ins.env[kAutoPanning].loop = 1;
                    VoiceControl c;
                    c.ampTap = tap != 0;
                    std::vector<float> l1, r1, l2, r2;
                    const Run a = renderSt(ins, left.data(), right.data(), false, c, 32, &l1, &r1);
                    const Run b = renderSt(ins, left.data(), right.data(), true, c, 32, &l2, &r2);
                    ++cases;
                    const bool ampOk = !tap || bitsEqual(a.amp, b.amp);
                    if (!ampOk || !bitsEqual(a.ds, b.ds) || !bitsEqual(a.rs, b.rs) || !bitsEqual(a.ml, b.ml) ||
                        !bitsEqual(a.mr, b.mr) || !bitsEqual(l1, l2) || !bitsEqual(r1, r2)) {
                        if (bad < 5)
                            std::printf("    differ: mode %d filt %d sends %d tap %d\n", mode, filt, sends, tap);
                        ++bad;
                    }
                }
    std::printf("  %d stereo configurations compared, %d differ\n", cases, bad);
    CHECK_EQ(bad, 0);
}
