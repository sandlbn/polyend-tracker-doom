// Line-input dynamics DYN0/DYN1 (spec 04 §5.6): gate thresholds and
// hysteresis, compressor curve, make-up, idle/bypass behaviour, engine wiring.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "audio/effects.h"
#include "audio/engine.h"
#include "test.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr float kPiF = 3.14159265f;
float lin(float db) { return std::pow(10.0f, db / 20.0f); }

// Feeds `blocks` blocks of constant input `v`; returns the last output sample.
float feedDc(LineDynamics& d, float v, int blocks) {
    float x[kBlock], y[kBlock];
    for (auto& s : x) s = v;
    for (int b = 0; b < blocks; ++b) d.process(x, y);
    return y[kBlock - 1];
}
double gainDb(LineDynamics& d, float levelDb) {
    const float v = lin(levelDb);
    return 20.0 * std::log10(double(feedDc(d, v, 40)) / double(v));
}

}  // namespace

TEST(linein_dyn_static_curve) {
    using D = LineDynamics;
    CHECK_NEAR(D::staticGainDb(-60.0f), 0.0, 1e-6);
    CHECK_NEAR(D::staticGainDb(-43.0f), 0.0, 1e-6);   // knee start
    // Soft knee: quadratic between -43 and -37 dB, continuous at both ends.
    CHECK_NEAR(D::staticGainDb(-40.0f), (1.0 / 35.0 - 1.0) * 9.0 / 12.0, 1e-4);
    CHECK_NEAR(D::staticGainDb(-37.0f), (1.0 / 35.0 - 1.0) * 3.0, 1e-4);
    CHECK_NEAR(D::staticGainDb(-36.999f), (1.0 / 35.0 - 1.0) * 3.001, 1e-3);
    // Above the knee: ratio 35 around -40 dB.
    CHECK_NEAR(D::staticGainDb(-20.0f), (1.0 / 35.0 - 1.0) * 20.0, 1e-4);
    CHECK_NEAR(D::staticGainDb(0.0f), (1.0 / 35.0 - 1.0) * 40.0, 1e-4);
    // Auto make-up: 0 dBFS comes out at -6 dB (headroom).
    CHECK_NEAR(D::kMakeupDb, 40.0 - 40.0 / 35.0 - 6.0, 1e-4);
}

TEST(linein_dyn_compressor_steady_state) {
    LineDynamics d;
    d.reset();
    // Gain = comp + make-up (gate open, limiter idle): out = -40 + (L+40)/35 + 32.857.
    for (float l : {-30.0f, -20.0f, -10.0f, 0.0f}) {
        const double want = double(LineDynamics::staticGainDb(l)) + LineDynamics::kMakeupDb;
        CHECK_NEAR(gainDb(d, l), want, 0.02);
        CHECK(d.gateOpen());
        CHECK_NEAR(d.gateDb(), 0.0, 1e-3);
    }
    // Output level stays in a 3.4 dB window over 30 dB of input (ratio 35).
    const double o30 = -30.0 + gainDb(d, -30.0f), o0 = 0.0 + gainDb(d, 0.0f);
    CHECK_NEAR(o0, -6.0, 0.02);
    CHECK(o0 - o30 < 1.0);
    // Attack is fast (gain falls), release slow (gain rises).
    LineDynamics a;
    a.reset();
    feedDc(a, lin(-30.0f), 40);
    float x[kBlock], y[kBlock];
    for (auto& s : x) s = lin(-5.0f);
    a.process(x, y);   // one block after a 25 dB jump: within 0.1 dB of the new gain
    CHECK_NEAR(a.compDb(), LineDynamics::staticGainDb(-5.0f), 0.1);
    for (auto& s : x) s = lin(-30.0f);
    a.process(x, y);   // falling level: detector and gain release over several blocks
    CHECK(a.compDb() < LineDynamics::staticGainDb(-30.0f) - 1.0f);
}

TEST(linein_dyn_gate_threshold_hysteresis) {
    LineDynamics d;
    d.reset();
    // Closed: -35 dB is between the thresholds, so it does not open the gate.
    feedDc(d, lin(-35.0f), 20);
    CHECK(!d.gateOpen());
    CHECK(d.gateDb() < -100.0f);
    CHECK(std::fabs(feedDc(d, lin(-35.0f), 1)) < lin(-35.0f) * lin(-60.0f));
    // -31.9 dB opens it (>= -32 dB).
    feedDc(d, lin(-31.9f), 20);
    CHECK(d.gateOpen());
    CHECK_NEAR(d.gateDb(), 0.0, 1e-3);
    // Back to -35 dB: hysteresis keeps it open.
    feedDc(d, lin(-35.0f), 20);
    CHECK(d.gateOpen());
    // -37.9 dB still open; -38.1 dB closes.
    feedDc(d, lin(-37.9f), 20);
    CHECK(d.gateOpen());
    feedDc(d, lin(-38.1f), 1);
    CHECK(!d.gateOpen());
    feedDc(d, lin(-38.1f), 40);
    CHECK(d.gateDb() < -100.0f);
    // Just below -32 dB does not reopen it.
    feedDc(d, lin(-32.1f), 20);
    CHECK(!d.gateOpen());
    // A sine whose peaks reach -20 dB keeps the gate open through its zero
    // crossings (peak detector), so the output has no gaps.
    LineDynamics s;
    s.reset();
    float x[kBlock], y[kBlock];
    uint32_t n = 0;
    float minAbsRatio = 1e9f;
    for (int b = 0; b < 40; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i, ++n) x[i] = lin(-20.0f) * std::sin(2.0f * kPiF * 100.0f * float(n) / 44100.0f);
        s.process(x, y);
        CHECK(s.gateOpen());
        if (b >= 4)
            for (uint32_t i = 0; i < kBlock; ++i)
                if (std::fabs(x[i]) > 1e-3f) minAbsRatio = std::fmin(minAbsRatio, y[i] / x[i]);
    }
    CHECK(minAbsRatio > 1.0f);   // never gated, always made up
}

TEST(linein_dyn_limiter) {
    // A step from quiet to full scale: the compressor's attack lags, the
    // instantaneous limiter holds the output at or below -3 dBFS.
    LineDynamics d;
    d.reset();
    feedDc(d, lin(-31.0f), 40);
    float x[kBlock], y[kBlock];
    for (auto& s : x) s = 1.0f;
    d.process(x, y);
    float pk = 0.0f;
    for (float v : y) pk = std::fmax(pk, std::fabs(v));
    CHECK(pk <= lin(-3.0f) * 1.001f);
    CHECK(pk > lin(-7.0f));
}

TEST(linein_dyn_idle_bypass) {
    LineDynamics d;
    d.reset();
    CHECK(d.settled());
    d.idle();   // settled: no state change
    CHECK(d.settled());
    CHECK(!d.gateOpen());
    feedDc(d, lin(-10.0f), 10);
    CHECK(!d.settled());
    CHECK(d.gateOpen());
    // Silence: closes and relaxes in closed form, then settles (zero cost).
    int blocks = 0;
    while (!d.settled() && blocks < 1000) {
        d.idle();
        ++blocks;
    }
    CHECK(d.settled());
    CHECK(blocks < 100);
    CHECK(!d.gateOpen());
    CHECK_NEAR(d.compDb(), 0.0, 1e-6);
    CHECK_NEAR(d.gateDb(), LineDynamics::kFloorDb, 1e-6);
    // Idle matches the per-sample path for zero input (same final regime).
    LineDynamics p;
    p.reset();
    feedDc(p, lin(-10.0f), 10);
    feedDc(p, 0.0f, 100);
    CHECK(!p.gateOpen());
    CHECK(p.gateDb() < LineDynamics::kFloorDb + 0.01f);
    CHECK(p.compDb() > -0.001f);
}

namespace {
struct EngRig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(1u << 16);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    EngRig() { CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get())); }
    // Peak |output| over `blocks` blocks of a line-in sine at `db` dBFS.
    int peak(float db, int blocks) {
        int16_t in[kBlock * 2], out[kBlock * 2];
        int pk = 0;
        for (int b = 0; b < blocks; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float v = lin(db) * std::sin(2.0f * kPiF * 441.0f * float(n++) / 44100.0f);
                in[2 * i] = in[2 * i + 1] = int16_t(v * 32767.0f);
            }
            eng->setLineInput(in);
            eng->renderLive(out);
            for (int16_t v : out) pk = std::max(pk, std::abs(int(v)));
        }
        return pk;
    }
    uint32_t n = 0;
};
}  // namespace

TEST(linein_dyn_engine_send_path) {
    // Direct line monitoring muted, pre-fader reverb send: what comes out is
    // the reverb return of the gated/compressed line-in.
    EngRig r;
    MixerSettings& m = r.eng->settings();
    m.lineInMute = true;
    m.postFaderSends = false;
    m.lineInReverbSend = 100;
    // -50 dBFS: below the gate, nothing reaches the reverb.
    CHECK_EQ(r.peak(-50.0f, 200), 0);
    // -20 dBFS: gate open, the reverb returns sound.
    CHECK(r.peak(-20.0f, 200) > 100);
    // Send off: the dynamics relax and the reverb tail dies out.
    m.lineInReverbSend = 0;
    r.peak(-20.0f, 3000);
    CHECK_EQ(r.peak(-20.0f, 50), 0);
}

TEST(linein_dyn_direct_path_untouched) {
    // The direct MSUM line path is not processed: -50 dBFS passes at unity
    // gain (TRK[86] = 1) with the sends on.
    EngRig r;
    MixerSettings& m = r.eng->settings();
    m.lineInDelaySend = 0;
    m.lineInReverbSend = 0;
    const int pk = r.peak(-50.0f, 20);
    CHECK(std::abs(pk - int(lin(-50.0f) * 32767.0f)) <= 2);
}
