// INTERNAL test engine (2.x WP-E) [OT]: one sine oscillator with an
// attack / decay / sustain / release envelope. Used by the framework tests,
// the synth4 bench and, until the WP-F engines land, to make ACD / VAP /
// PERC slots audible (synth_registry.cpp). Not a Tracker Mini model.
//
// Envelope times come from the patch's "Amp Env" parameters when the engine
// has them (spec 21 §8: ACD 15-18, FAT 11-14, VAP 15-18, WTFM 20-23), else
// a short percussive envelope. The sine is a parabolic approximation of a
// phase accumulator (no tables, no libm in the sample loop).
#include <cmath>

#include "audio/synth_engine.h"

namespace ot {
namespace audio {

namespace {

struct TestVoice {
    float phase;      // -1..1
    float env;        // 0..1
    float pitch;      // note pitch (semitones)
    uint8_t stage;    // 0 idle, 1 attack, 2 decay/sustain, 3 release
};

struct Env {
    float a, d, s, r;
};

Env envOf(const SynthParams& p) {
    int base = -1;
    switch (p.engine) {
        case kSynthEngineAcd: base = 15; break;
        case kSynthEngineFat: base = 11; break;
        case kSynthEngineVap: base = 15; break;
        case kSynthEngineWtfm: base = 20; break;
        default: break;
    }
    if (base < 0 || base + 3 >= p.count) return Env{0.002f, 0.25f, 0.0f, 0.15f};
    return Env{p.end[base], p.end[base + 1], p.end[base + 2], p.end[base + 3]};
}

// Per-sample coefficient reaching ~1/e^5 of the distance in t seconds.
inline float expCoef(float t) {
    if (t < 0.0005f) return 1.0f;
    return 1.0f - std::exp(-5.0f / (t * kSampleRate));
}

class TestEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEngineTest; }
    const char* name() const override { return "TEST"; }

    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        (void)p;
        TestVoice& t = v.as<TestVoice>();
        t.pitch = n.pitch;
        if (!n.overlap) t.stage = 1;   // legato keeps the envelope running
        v.level = t.env;
    }
    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        v.as<TestVoice>().stage = 3;
    }
    bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override {
        TestVoice& t = v.as<TestVoice>();
        if (t.stage == 0) return false;
        const Env e = envOf(p);
        const float hz0 = synthPitchHz(t.pitch + v.bendStart), hz1 = synthPitchHz(t.pitch + v.bend);
        float inc = 2.0f * hz0 / kSampleRate;
        const float incStep = (2.0f * hz1 / kSampleRate - inc) * (1.0f / float(kBlock));
        const float attack = e.a < 0.0005f ? 1.0f : 1.0f / (e.a * kSampleRate);
        const float kd = expCoef(e.d), kr = expCoef(e.r);
        const float sus = e.s < 0.0f ? 0.0f : (e.s > 1.0f ? 1.0f : e.s);
        float ph = t.phase, env = t.env, g = g0;
        uint8_t st = t.stage;
        for (uint32_t i = 0; i < kBlock; ++i) {
            ph += inc;
            inc += incStep;
            if (ph >= 1.0f) ph -= 2.0f;
            const float x = 4.0f * ph * (1.0f - std::fabs(ph));   // parabolic sine
            if (st == 1) {
                env += attack;
                if (env >= 1.0f) {
                    env = 1.0f;
                    st = 2;
                }
            } else if (st == 2) {
                env += (sus - env) * kd;
            } else {
                env -= env * kr;
            }
            const float y = x * env * g;
            g += gInc;
            L[i] += y;
            R[i] += y;
        }
        t.phase = ph;
        t.env = env;
        t.stage = st;
        v.level = env;
        if (st == 3 && env < 1.0e-4f) {
            t.stage = 0;
            return false;
        }
        if (st == 2 && sus == 0.0f && env < 1.0e-4f) {
            t.stage = 0;
            return false;
        }
        return true;
    }
};

TestEngine gTest;

}  // namespace

ISynthEngine* synthTestEngine() { return &gTest; }

}  // namespace audio
}  // namespace ot
