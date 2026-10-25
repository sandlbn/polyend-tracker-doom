// ACD synth engine (2.x WP-F; audio/engines/acd.*, spec 21 §8.2): every
// parameter has a measurable effect within its range, glide / legato modes,
// envelopes and voice end, silence settling to exact zero, stability over
// random and extreme parameter sets, the engine leaving Volume to the
// framework, and the factory ACD patches ($OT_VENDOR_FIXTURES, skipped when
// absent) rendering without NaN or blow-ups.
#include "test.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/engines/acd.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

namespace fs = std::filesystem;
using namespace ot;
using namespace ot::audio;
using namespace ot::audio::acd;

namespace {

using Buf = std::vector<float>;

ISynthEngine* eng() { return synthEngineFor(kSynthEngineAcd); }

// Init patch parameters with overrides.
struct Params {
    SynthParams p;
    Params() {
        SynthPatch patch;
        patchInit(kSynthEngineAcd, &patch);
        const uint8_t m[kSynthSeqMacros] = {};
        p.engine = kSynthEngineAcd;
        p.count = synthEffectiveParams(patch, m, 1.0f, p.end);
        std::memcpy(p.start, p.end, sizeof(p.end));
        p.serial = 1;
    }
    Params& set(int i, float v) {
        p.start[i] = p.end[i] = v;
        ++p.serial;
        return *this;
    }
    // A plain sustained voice: full sustain, open filter, no key tracking.
    static Params open() {
        Params q;
        q.set(kAmpS, 1.0f).set(kCutoff, 20000.0f).set(kResonance, 0.0f).set(kFilterType, kSvf12);
        q.set(kNoteTrack, 0.0f).set(kFilterEnv, 0.0f);
        return q;
    }
};

// One voice in one slot.
struct Rig {
    SynthSlotScratch scratch;
    SynthVoice v;
    bool alive = true;
    int blocks = 0;
    Rig() {
        eng()->slotReset(scratch);
        v.slotScratch = &scratch;
        std::memset(v.state, 0, sizeof(v.state));
    }
    void on(const SynthParams& p, float pitch, bool reused = false, bool overlap = false) {
        SynthNote n;
        n.pitch = pitch;
        n.note = uint8_t(std::clamp(int(pitch), 0, 127));
        n.reused = reused;
        n.overlap = overlap;
        if (!reused) {
            std::memset(v.state, 0, sizeof(v.state));
            alive = true;
        }
        v.gate = true;
        eng()->noteOn(v, p, n);
    }
    void off(const SynthParams& p) {
        v.gate = false;
        eng()->noteOff(v, p);
    }
    // Render `frames` (rounded up to blocks) into out (appended).
    void run(const SynthParams& p, float seconds, Buf& out, float gain = 1.0f) {
        const int nb = int(seconds * kSampleRate / float(kBlock)) + 1;
        float L[kBlock], R[kBlock];
        for (int b = 0; b < nb; ++b) {
            std::fill(L, L + kBlock, 0.0f);
            std::fill(R, R + kBlock, 0.0f);
            if (alive) {
                eng()->prepare(scratch, p);
                alive = eng()->render(v, p, gain, 0.0f, L, R);
                ++blocks;
            }
            for (uint32_t i = 0; i < kBlock; ++i) out.push_back(L[i]);
        }
    }
};

Buf play(const SynthParams& p, float pitch = 48.0f, float seconds = 0.5f) {
    Rig r;
    Buf out;
    r.on(p, pitch);
    r.run(p, seconds, out);
    return out;
}

Buf slice(const Buf& b, float t0, float t1) {
    const size_t a = size_t(t0 * kSampleRate), e = std::min(b.size(), size_t(t1 * kSampleRate));
    return Buf(b.begin() + long(std::min(a, e)), b.begin() + long(e));
}
double rms(const Buf& b) {
    double s = 0;
    for (float x : b) s += double(x) * x;
    return b.empty() ? 0.0 : std::sqrt(s / double(b.size()));
}
double peak(const Buf& b) {
    double m = 0;
    for (float x : b) m = std::max(m, double(std::fabs(x)));
    return m;
}
// High-frequency proxy: rms of the first difference over rms.
double bright(const Buf& b) {
    double s = 0;
    for (size_t i = 1; i < b.size(); ++i) s += double(b[i] - b[i - 1]) * (b[i] - b[i - 1]);
    const double r = rms(b);
    return r <= 0 ? 0.0 : std::sqrt(s / double(b.size())) / r;
}
// Relative difference of two renders.
double diff(const Buf& a, const Buf& b) {
    const size_t n = std::min(a.size(), b.size());
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += double(a[i] - b[i]) * (a[i] - b[i]);
    const double d = std::sqrt(s / double(n)), m = std::max(rms(a), rms(b));
    return m <= 0 ? 0.0 : d / m;
}
// Goertzel magnitude (normalised, Hann window) at hz.
double tone(const Buf& b, double hz) {
    const double w = 2.0 * 3.14159265358979 * hz / kSampleRate, c = 2.0 * std::cos(w);
    const double wn = 2.0 * 3.14159265358979 / double(b.size());
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double s0 = b[i] * (0.5 - 0.5 * std::cos(wn * double(i))) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(b.size());
}
// Frequency from upward zero crossings (bright waveforms with one crossing per cycle).
double freq(const Buf& b) {
    int n = 0;
    size_t first = 0, last = 0;
    for (size_t i = 1; i < b.size(); ++i)
        if (b[i - 1] < 0.0f && b[i] >= 0.0f) {
            if (!n) first = i;
            last = i;
            ++n;
        }
    return n < 2 ? 0.0 : double(n - 1) * kSampleRate / double(last - first);
}
double hz(double pitch) { return 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0); }

bool finite(const Buf& b) {
    for (float x : b)
        if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace

// --- registry and contract -------------------------------------------------------------

TEST(acd_registered) {
    ISynthEngine* e = synthEngineFor(kSynthEngineAcd);
    CHECK(e != nullptr);
    CHECK_EQ(e->id(), int32_t(kSynthEngineAcd));
    CHECK(std::strcmp(e->name(), "ACD") == 0);
    CHECK(e != synthTestEngine());
    CHECK_EQ(synthParamCount(kSynthEngineAcd), int(kParamCount));
}

TEST(acd_init_patch_sounds_and_ends) {
    Params q;   // Init: saw, LP SVF 24 400 Hz, decay 0.5 s, sustain 0
    Rig r;
    Buf out;
    r.on(q.p, 48.0f);
    r.run(q.p, 3.0f, out);
    CHECK(finite(out));
    CHECK(rms(slice(out, 0.0, 0.2)) > 0.02);
    CHECK(peak(out) < 2.0);
    CHECK(!r.alive);                      // decayed to the 0 sustain: finished
    CHECK(r.blocks * kBlock < uint32_t(1.5f * kSampleRate));
    CHECK_NEAR(freq(slice(play(Params::open().p, 57.0f), 0.05, 0.4)), 220.0, 2.0);
}

TEST(acd_volume_left_to_framework) {
    Params a = Params::open(), b = Params::open();
    b.set(kVolume, 0.25f);
    CHECK(diff(play(a.p), play(b.p)) < 1e-6);
    // The gain ramp passed to render() scales the output.
    Rig r1, r2;
    Buf o1, o2;
    r1.on(a.p, 48.0f);
    r2.on(a.p, 48.0f);
    r1.run(a.p, 0.2f, o1, 1.0f);
    r2.run(a.p, 0.2f, o2, 0.5f);
    CHECK_NEAR(rms(o2) / rms(o1), 0.5, 1e-3);
}

// --- oscillator parameters -------------------------------------------------------------

TEST(acd_osc_mix_levels) {
    for (int i : {int(kSawMix), int(kSquareMix), int(kSubMix)}) {
        Params q = Params::open();
        q.set(kSawMix, 0.0f).set(kSquareMix, 0.0f).set(kSubMix, 0.0f);
        const double silent = rms(play(q.p));
        q.set(i, 0.5f);
        const double half = rms(play(q.p));
        q.set(i, 1.0f);
        const double full = rms(play(q.p));
        CHECK(silent < 1e-6);
        CHECK(half > 0.05);
        CHECK_NEAR(full / half, 2.0, 0.05);
    }
    // The sub sits an octave below (-1 oct modes) or two (-2 oct modes).
    Params q = Params::open();
    q.set(kSawMix, 0.0f).set(kSubMix, 1.0f);
    const double f0 = hz(60.0);
    for (int m = 0; m < 6; ++m) {
        q.set(kSubMode, float(m));
        Buf b = slice(play(q.p, 60.0f), 0.05, 0.45);
        const double below = m < 3 ? f0 / 2 : f0 / 4;
        CHECK(tone(b, below) > 4.0 * tone(b, f0 * 1.37));
        CHECK(tone(b, below) > 0.05);
    }
}

TEST(acd_sub_modes_differ) {
    Params q = Params::open();
    q.set(kSawMix, 0.0f).set(kSubMix, 1.0f);
    Buf m[6];
    for (int i = 0; i < 6; ++i) {
        q.set(kSubMode, float(i));
        m[i] = slice(play(q.p, 60.0f), 0.05, 0.45);
    }
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j) CHECK(diff(m[i], m[j]) > 0.1);
    // DC-free in every mode (an open filter passes DC).
    for (int i = 0; i < 6; ++i) {
        double mean = 0;
        for (float x : m[i]) mean += x;
        CHECK(std::fabs(mean / double(m[i].size())) < 0.02 * rms(m[i]));
    }
    // Square has no 2nd harmonic of the sub, the pulse has; the triangle is darkest.
    const double fs = hz(60.0) / 2;
    CHECK(tone(m[1], 2 * fs) > 5.0 * tone(m[0], 2 * fs));
    CHECK(bright(m[2]) < 0.5 * bright(m[0]));
}

TEST(acd_noise_mix_pre_and_post_filter) {
    Params q = Params::open();
    q.set(kSawMix, 0.0f).set(kCutoff, 300.0f);
    q.set(kNoiseMix, 0.0f);
    CHECK(rms(play(q.p)) < 1e-6);
    q.set(kNoiseMix, 1.0f);
    Buf pre = play(q.p);
    q.set(kNoiseMix, -1.0f);
    Buf post = play(q.p);
    q.set(kNoiseMix, -0.5f);
    Buf postHalf = play(q.p);
    CHECK(rms(pre) > 0.005);
    CHECK(rms(post) > 0.05);
    CHECK(bright(post) > 3.0 * bright(pre));   // + goes through the 300 Hz filter, - does not
    CHECK_NEAR(rms(postHalf) / rms(post), 0.5, 0.05);
}

TEST(acd_pulse_width) {
    Params q = Params::open();
    q.set(kSawMix, 0.0f).set(kSquareMix, 1.0f);
    const double f = hz(48.0);
    q.set(kPw, 0.5f);
    Buf sq = slice(play(q.p), 0.05, 0.45);
    q.set(kPw, 0.2f);
    Buf narrow = slice(play(q.p), 0.05, 0.45);
    q.set(kPw, 0.0f);
    Buf thinnest = slice(play(q.p), 0.05, 0.45);
    q.set(kPw, 1.0f);
    Buf widest = slice(play(q.p), 0.05, 0.45);
    CHECK(tone(sq, 2 * f) < 0.05 * tone(sq, f));      // square: no even harmonics
    CHECK(tone(narrow, 2 * f) > 0.3 * tone(narrow, f));
    CHECK(rms(thinnest) < 0.6 * rms(sq));             // thin pulse: less energy
    CHECK(rms(thinnest) > 0.01);                      // but not silent (3 % floor)
    CHECK(rms(widest) > 0.01);
}

TEST(acd_pw_modulation) {
    Params q = Params::open();
    q.set(kSawMix, 0.0f).set(kSquareMix, 1.0f).set(kPw, 0.5f).set(kLfoHz, 3.0f).set(kLfoRetrig, kNote);
    Buf base = play(q.p);
    q.set(kPwLfo, 0.4f);
    Buf lfo = play(q.p);
    q.set(kPwLfo, -0.4f);
    Buf lfoNeg = play(q.p);
    CHECK(diff(base, lfo) > 0.2);
    CHECK(diff(lfo, lfoNeg) > 0.2);
    q.set(kPwLfo, 0.0f).set(kPwEnv, -0.45f).set(kModS, 0.0f).set(kModD, 0.3f);
    Buf env = play(q.p);
    CHECK(diff(slice(base, 0, 0.1), slice(env, 0, 0.1)) > 0.2);   // narrow at the start
    CHECK(diff(slice(base, 0.45, 0.5), slice(env, 0.45, 0.5)) < 0.1);   // back to square after the decay
}

TEST(acd_pitch_lfo_octaves) {
    Params q = Params::open();
    q.set(kLfoWave, kSquare).set(kLfoHz, 0.5f).set(kLfoRetrig, kNote).set(kPitchLfo, 1.0f);
    // Square LFO starts high: +1 octave for the first half cycle (1 s).
    CHECK_NEAR(freq(slice(play(q.p, 57.0f, 0.4f), 0.05, 0.35)), 440.0, 4.0);
    q.set(kPitchLfo, -0.5f);
    CHECK_NEAR(freq(slice(play(q.p, 57.0f, 0.4f), 0.05, 0.35)), 220.0 / std::sqrt(2.0), 2.0);
    q.set(kPitchLfo, 0.0f);
    CHECK_NEAR(freq(slice(play(q.p, 57.0f, 0.4f), 0.05, 0.35)), 220.0, 2.0);
}

// --- filter ----------------------------------------------------------------------------

TEST(acd_cutoff_brightness_all_types) {
    for (int t = 0; t < 3; ++t) {
        Params q = Params::open();
        q.set(kFilterType, float(t)).set(kResonance, 0.3f);
        double last = 0;
        for (float fc : {100.0f, 400.0f, 1600.0f, 6400.0f, 20000.0f}) {
            q.set(kCutoff, fc);
            const double b = bright(slice(play(q.p, 36.0f), 0.05, 0.45));
            CHECK(b > last * (fc > 10000.0f ? 1.0 : 1.15));
            last = b;
        }
        q.set(kCutoff, 20.0f);
        CHECK(rms(slice(play(q.p, 60.0f), 0.1, 0.45)) < 0.05);   // 20 Hz: nearly closed
    }
}

TEST(acd_resonance_peak) {
    for (int t = 0; t < 3; ++t) {
        Params q = Params::open();
        q.set(kFilterType, float(t)).set(kCutoff, 1000.0f);
        double last = -1;
        for (float res : {0.0f, 0.5f, 1.0f, 1.5f}) {
            q.set(kResonance, res);
            Buf b = slice(play(q.p, 36.0f), 0.1, 0.45);   // 65.4 Hz saw, 15th harmonic ~ 981 Hz
            const double rel = tone(b, hz(36.0) * 15) / tone(b, hz(36.0));
            CHECK(rel > last * 1.2);
            CHECK(peak(b) < 4.0);
            last = rel;
        }
    }
}

TEST(acd_filter_types_slopes) {
    Params q = Params::open();
    q.set(kCutoff, 300.0f).set(kResonance, 0.0f);
    Buf t[3];
    for (int i = 0; i < 3; ++i) {
        q.set(kFilterType, float(i));
        t[i] = slice(play(q.p, 36.0f), 0.1, 0.45);
    }
    const double f = hz(36.0);
    // At ~12x the cutoff the 24 dB types are far below the 12 dB one.
    const double r12 = tone(t[0], 55 * f) / tone(t[0], f), r24 = tone(t[1], 55 * f) / tone(t[1], f),
                 rl = tone(t[2], 55 * f) / tone(t[2], f);
    CHECK(r24 < 0.25 * r12);
    CHECK(rl < 0.25 * r12);
    CHECK(diff(t[1], t[2]) > 0.05);
}

TEST(acd_note_track) {
    Params q = Params::open();
    q.set(kCutoff, 1500.0f).set(kFilterType, kSvf24);
    double b[5];
    const float tr[5] = {2.0f, 1.0f, 0.0f, -1.0f, -2.0f};
    for (int i = 0; i < 5; ++i) {
        q.set(kNoteTrack, tr[i]);
        b[i] = bright(slice(play(q.p, 36.0f), 0.1, 0.45));   // two octaves below C4
    }
    for (int i = 1; i < 5; ++i) CHECK(b[i] > b[i - 1] * 1.1);
    // Around C4 the tracking does nothing.
    q.set(kNoteTrack, 0.0f);
    Buf c0 = play(q.p, 60.0f);
    q.set(kNoteTrack, 2.0f);
    CHECK(diff(c0, play(q.p, 60.0f)) < 0.01);
}

TEST(acd_filter_env_amount) {
    Params q = Params::open();
    q.set(kCutoff, 300.0f).set(kModD, 0.2f).set(kModS, 0.0f);
    q.set(kFilterEnv, 0.0f);
    Buf flat = play(q.p);
    q.set(kFilterEnv, 0.3f);
    Buf up = play(q.p);
    q.set(kFilterEnv, 0.02f);
    Buf small = play(q.p);
    q.set(kFilterEnv, -0.3f);
    q.set(kCutoff, 3000.0f);
    Buf down = play(q.p);
    q.set(kFilterEnv, 0.0f);
    Buf downRef = play(q.p);
    CHECK(bright(slice(up, 0, 0.05)) > 2.0 * bright(slice(flat, 0, 0.05)));
    CHECK(bright(slice(small, 0, 0.05)) > 1.1 * bright(slice(flat, 0, 0.05)));   // small amounts matter
    CHECK(diff(slice(up, 0.45, 0.5), slice(flat, 0.45, 0.5)) < 0.05);            // envelope over
    CHECK(bright(slice(down, 0, 0.05)) < 0.6 * bright(slice(downRef, 0, 0.05)));
}

TEST(acd_filter_lfo_amount) {
    Params q = Params::open();
    q.set(kCutoff, 600.0f).set(kLfoWave, kSquare).set(kLfoHz, 2.0f).set(kLfoRetrig, kNote);
    q.set(kFilterLfo, 0.0f);
    Buf ref = play(q.p, 36.0f, 0.5f);
    q.set(kFilterLfo, 0.2f);
    Buf m = play(q.p, 36.0f, 0.5f);
    // First half cycle (LFO +1) brighter, second (-1) darker.
    CHECK(bright(slice(m, 0.05, 0.24)) > 1.5 * bright(slice(ref, 0.05, 0.24)));
    CHECK(bright(slice(m, 0.3, 0.49)) < 0.8 * bright(slice(ref, 0.3, 0.49)));
}

// --- envelopes ---------------------------------------------------------------------

TEST(acd_amp_envelope) {
    Params q = Params::open();
    // Attack
    q.set(kAmpA, 0.0f);
    Buf fast = play(q.p);
    q.set(kAmpA, 0.4f);
    Buf slow = play(q.p);
    CHECK(rms(slice(slow, 0, 0.05)) < 0.3 * rms(slice(fast, 0, 0.05)));
    CHECK(rms(slice(slow, 0.42, 0.5)) > 0.8 * rms(slice(fast, 0.42, 0.5)));
    // Decay and sustain
    q.set(kAmpA, 0.0f).set(kAmpS, 0.0f).set(kAmpD, 0.1f);
    Buf d1 = play(q.p);
    q.set(kAmpD, 1.0f);
    Buf d2 = play(q.p);
    CHECK(rms(slice(d2, 0.2, 0.3)) > 3.0 * rms(slice(d1, 0.2, 0.3)));
    q.set(kAmpS, 0.5f);
    Buf s5 = play(q.p, 48.0f, 6.0f);
    q.set(kAmpS, 1.0f);
    Buf s10 = play(q.p, 48.0f, 6.0f);
    CHECK_NEAR(rms(slice(s5, 5.5, 6.0)) / rms(slice(s10, 5.5, 6.0)), 0.5, 0.02);
    // Release and voice end
    for (float rel : {0.0f, 0.05f, 1.0f, 10.0f}) {
        q.set(kAmpS, 1.0f).set(kAmpR, rel);
        Rig r;
        Buf out;
        r.on(q.p, 48.0f);
        r.run(q.p, 0.1f, out);
        r.off(q.p);
        const size_t at = out.size();
        r.run(q.p, 2.5f * rel + 0.1f, out);
        CHECK(!r.alive);
        CHECK(r.v.level < 1e-3f);
        if (rel >= 1.0f) {
            Buf tail(out.begin() + long(at), out.end());
            CHECK(rms(slice(tail, 0.4f * rel, 0.5f * rel)) > 0.05 * rms(slice(out, 0.05, 0.1)));
        }
    }
}

TEST(acd_mod_envelope_stages) {
    // Mod Env drives the filter: watch the brightness.
    Params q = Params::open();
    q.set(kCutoff, 200.0f).set(kFilterEnv, 0.5f).set(kModA, 0.0f).set(kModD, 0.1f).set(kModS, 0.0f);
    Buf a0 = play(q.p);
    q.set(kModA, 0.3f);
    Buf a3 = play(q.p);
    CHECK(bright(slice(a3, 0, 0.03)) < 0.6 * bright(slice(a0, 0, 0.03)));
    q.set(kModA, 0.0f).set(kModD, 1.0f);
    Buf d1 = play(q.p);
    CHECK(bright(slice(d1, 0.2, 0.3)) > 1.5 * bright(slice(a0, 0.2, 0.3)));
    q.set(kModD, 0.1f).set(kModS, 0.6f);
    Buf s6 = play(q.p);
    CHECK(bright(slice(s6, 0.4, 0.5)) > 1.5 * bright(slice(a0, 0.4, 0.5)));
    // Release: after the note off the brightness falls with the Mod release.
    for (float mr : {0.02f, 10.0f}) {
        q.set(kModS, 1.0f).set(kModR, mr).set(kAmpR, 2.0f);
        Rig r;
        Buf out;
        r.on(q.p, 48.0f);
        r.run(q.p, 0.1f, out);
        r.off(q.p);
        Buf rel;
        r.run(q.p, 0.3f, rel);
        if (mr < 0.5f)
            CHECK(bright(slice(rel, 0.2, 0.3)) < 0.6 * bright(slice(out, 0.05, 0.1)));
        else
            CHECK(bright(slice(rel, 0.2, 0.3)) > 0.8 * bright(slice(out, 0.05, 0.1)));
    }
}

// --- LFO ---------------------------------------------------------------------------

TEST(acd_lfo_frequency_and_shapes) {
    Params q = Params::open();
    q.set(kCutoff, 500.0f).set(kFilterLfo, 0.3f).set(kLfoRetrig, kNote);
    Buf w[6];
    for (int i = 0; i < 6; ++i) {
        q.set(kLfoWave, float(i));
        w[i] = play(q.p, 36.0f, 1.0f);
    }
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j) CHECK(diff(w[i], w[j]) > 0.05);
    q.set(kLfoWave, kSine);
    Buf f[3];
    const float hzs[3] = {0.1f, 4.0f, 100.0f};
    for (int i = 0; i < 3; ++i) {
        q.set(kLfoHz, hzs[i]);
        f[i] = play(q.p, 36.0f, 1.0f);
    }
    CHECK(diff(f[0], f[1]) > 0.1);
    CHECK(diff(f[1], f[2]) > 0.1);
}

TEST(acd_lfo_retrigger_modes) {
    Params q = Params::open();
    q.set(kPitchLfo, 0.1f).set(kLfoWave, kTri).set(kLfoHz, 3.3f);
    // Note: every note starts the LFO at phase 0 -> identical starts.
    q.set(kLfoRetrig, kNote);
    Rig r;
    Buf a, b, c;
    r.on(q.p, 48.0f);
    r.run(q.p, 0.13f, a);
    r.off(q.p);
    r.on(q.p, 48.0f, true, false);
    r.run(q.p, 0.13f, b);
    // Free running: the slot's phase keeps going -> the second note differs.
    Params fr = q;
    fr.set(kLfoRetrig, kFree);
    Rig rf;
    Buf fa, fb;
    rf.on(fr.p, 48.0f);
    rf.run(fr.p, 0.13f, fa);
    rf.on(fr.p, 48.0f);
    rf.run(fr.p, 0.13f, fb);
    CHECK(diff(slice(fa, 0, 0.1), slice(fb, 0, 0.1)) > 0.05);
    // One shot: one cycle, then the pitch holds.
    Params os = q;
    os.set(kLfoRetrig, kOneShot);
    Buf o = play(os.p, 57.0f, 1.0f);
    Buf n = play(q.p, 57.0f, 1.0f);
    CHECK_NEAR(freq(slice(o, 0.5, 0.9)), 220.0, 1.5);
    CHECK(diff(slice(o, 0.5, 0.9), slice(n, 0.5, 0.9)) > 0.1);
    // Note mode retriggered: the two pitch tracks match (ignoring the phase of the saw).
    CHECK_NEAR(freq(slice(a, 0.01, 0.12)), freq(slice(b, 0.01, 0.12)), 1.0);
}

// --- glide ---------------------------------------------------------------------------

TEST(acd_glide_modes_and_time) {
    auto glideRun = [](int mode, float time, bool overlap, float& mid) {
        Params q = Params::open();
        q.set(kGlideMode, float(mode)).set(kGlideTime, time);
        Rig r;
        Buf a, b;
        r.on(q.p, 45.0f);   // 110 Hz
        r.run(q.p, 0.1f, a);
        if (!overlap) r.off(q.p);
        r.on(q.p, 57.0f, true, overlap);   // 220 Hz
        r.run(q.p, 0.8f, b);
        mid = float(freq(slice(b, 0.08, 0.12)));
        return freq(slice(b, 0.7, 0.8));
    };
    float mid;
    // No glide time: straight to the new pitch.
    CHECK_NEAR(glideRun(kAlways, 0.0f, true, mid), 220.0, 1.5);
    CHECK_NEAR(mid, 220.0, 3.0);
    // Glide 0.2 s: half way (in semitones) after 0.1 s, arrived later.
    CHECK_NEAR(glideRun(kAlways, 0.2f, true, mid), 220.0, 1.5);
    CHECK(mid > 140.0f && mid < 175.0f);
    float mid2;
    glideRun(kAlways, 0.6f, true, mid2);
    CHECK(mid2 < mid - 10.0f);   // longer time: slower
    // Always and Legato glide without overlap; the Overlap modes do not.
    glideRun(kAlways, 0.2f, false, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(kLegato, 0.2f, false, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(kOverlap, 0.2f, false, mid);
    CHECK_NEAR(mid, 220.0, 3.0);
    glideRun(kLegatoOverlap, 0.2f, false, mid);
    CHECK_NEAR(mid, 220.0, 3.0);
    glideRun(kOverlap, 0.2f, true, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(kLegatoOverlap, 0.2f, true, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    // A fresh voice (polyphony > 1) glides from the slot's previous note in Always.
    {
        Params q = Params::open();
        q.set(kGlideMode, kAlways).set(kGlideTime, 0.2f);
        Rig r;
        Buf a, b;
        r.on(q.p, 45.0f);
        r.run(q.p, 0.05f, a);
        SynthVoice v2;
        v2.slotScratch = &r.scratch;
        std::memset(v2.state, 0, sizeof(v2.state));
        SynthNote n;
        n.pitch = 57.0f;
        eng()->noteOn(v2, q.p, n);
        float L[kBlock], R[kBlock];
        for (int k = 0; k < 40; ++k) {
            std::fill(L, L + kBlock, 0.0f);
            eng()->prepare(r.scratch, q.p);
            eng()->render(v2, q.p, 1.0f, 0.0f, L, R);
            if (k >= 30) b.insert(b.end(), L, L + kBlock);
        }
        const double f = freq(b);   // ~0.1 s after the note
        CHECK(f > 140.0 && f < 175.0);
    }
}

TEST(acd_legato_skips_retrigger) {
    Params q = Params::open();
    q.set(kAmpA, 0.0f).set(kAmpD, 0.05f).set(kAmpS, 0.3f);
    for (int mode = 0; mode < 4; ++mode) {
        q.set(kGlideMode, float(mode));
        Rig r;
        Buf a, b;
        r.on(q.p, 48.0f);
        r.run(q.p, 0.5f, a);
        CHECK_NEAR(r.v.level, 0.3f, 0.01f);
        r.on(q.p, 50.0f, true, true);
        r.run(q.p, 0.01f, b);
        const bool legato = mode == kLegato || mode == kLegatoOverlap;
        if (legato)
            CHECK_NEAR(r.v.level, 0.3f, 0.01f);
        else
            CHECK(r.v.level > 0.6f);
    }
}

TEST(acd_bend_adds_to_pitch) {
    Params q = Params::open();
    Rig r;
    Buf out;
    r.on(q.p, 57.0f);
    r.v.bend = r.v.bendStart = 12.0f;
    r.run(q.p, 0.3f, out);
    CHECK_NEAR(freq(slice(out, 0.05, 0.3)), 440.0, 4.0);
}

// --- numerics ----------------------------------------------------------------------

TEST(acd_silence_settles_to_zero) {
    Params q = Params::open();
    q.set(kCutoff, 2000.0f).set(kResonance, 1.2f).set(kFilterType, kSvf24);
    for (int t = 0; t < 3; ++t) {
        q.set(kFilterType, float(t)).set(kSawMix, 1.0f);
        Rig r;
        Buf a;
        r.on(q.p, 48.0f);
        r.run(q.p, 0.2f, a);
        Params z = q;
        z.set(kSawMix, 0.0f);   // input silent, voice held
        Buf s;
        r.run(z.p, 6.0f, s);
        CHECK(r.alive);
        bool subnormal = false;
        for (float x : s)
            if (std::fpclassify(x) == FP_SUBNORMAL) subnormal = true;
        CHECK(!subnormal);
        Buf tail = slice(s, 5.5, 6.0);
        CHECK(peak(tail) == 0.0);
    }
}

TEST(acd_stability_random_and_extreme) {
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) * (1.0f / 16777216.0f);
    };
    int bad = 0, loud = 0;
    for (int it = 0; it < 1000; ++it) {
        SynthParams p;
        p.engine = kSynthEngineAcd;
        p.count = kParamCount;
        p.serial = uint32_t(it + 1);
        for (int i = 0; i < kParamCount; ++i) {
            const PatchParamRange* rg = synthParamRange(kSynthEngineAcd, i);
            auto pick = [&]() {
                const float u = rnd();
                if (u < 0.15f) return rg->min;
                if (u < 0.30f) return rg->max;
                float v = rg->min + (rg->max - rg->min) * rnd();
                // Times: favour short ones so the test also sees voice ends.
                if (i >= kAmpA && i <= kModR && i != kAmpS && i != kModS) v *= 0.05f;
                return v;
            };
            p.end[i] = pick();
            p.start[i] = (rnd() < 0.3f) ? pick() : p.end[i];
            p.moving = p.moving || p.start[i] != p.end[i];
        }
        Rig r;
        Buf out;
        const float pitch = rnd() * 127.0f;
        r.on(p, pitch, false, false);
        r.v.bend = r.v.bendStart = (rnd() - 0.5f) * 48.0f;
        r.run(p, 0.05f, out);
        // A second note on the same voice (mono reuse), then the release.
        r.on(p, rnd() * 127.0f, true, rnd() < 0.5f);
        r.run(p, 0.05f, out);
        r.off(p);
        r.run(p, 0.05f, out);
        if (!finite(out)) ++bad;
        if (peak(out) > 8.0) ++loud;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(loud, 0);
}

// --- factory patches ---------------------------------------------------------------

TEST(acd_factory_patches_render) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    fs::path dir = root ? fs::path(root) / "mini-patches-2.0" / "ACD" : fs::path();
    std::error_code ec;
    if (!root || !fs::is_directory(dir, ec)) {
        std::printf("  SKIP: no vendor ACD patches (OT_VENDOR_FIXTURES=%s)\n", root ? root : "unset");
        return;
    }
    std::vector<fs::path> files;
    for (auto& de : fs::directory_iterator(dir, ec))
        if (de.path().extension() == ".patch") files.push_back(de.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() > 40);
    int silent = 0;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static SynthPatch patch;
        CHECK(decodePatch(bytes.data(), bytes.size(), &patch) == PatchStatus::kOk);
        CHECK_EQ(patch.engine, int32_t(kSynthEngineAcd));
        uint8_t m[kSynthSeqMacros];
        for (int k = 0; k < kSynthSeqMacros; ++k) m[k] = synthMacroDefault(patch.macros[k].bipolar);
        SynthParams p;
        p.engine = kSynthEngineAcd;
        p.count = synthEffectiveParams(patch, m, 1.0f, p.end);
        std::memcpy(p.start, p.end, sizeof(p.end));
        p.serial = 1;
        Buf out;
        for (float pitch : {36.0f, 60.0f}) {
            Rig r;
            r.on(p, pitch);
            r.run(p, 0.7f, out);
            r.off(p);
            r.run(p, 0.3f, out);
        }
        const bool ok = finite(out) && peak(out) < 6.0;
        if (!ok) std::printf("  patch %s: finite=%d peak=%.2f\n", f.filename().c_str(), finite(out), peak(out));
        CHECK(ok);
        if (rms(out) < 1e-3) {
            ++silent;
            std::printf("  patch %s: silent (rms %.5f)\n", f.filename().c_str(), rms(out));
        }
    }
    CHECK_EQ(silent, 0);
}
