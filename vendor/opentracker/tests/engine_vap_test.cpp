// VAP engine (2.x WP-F, audio/engines/vap.*): every parameter audibly does
// something across its range, polyphonic chords, patch glide, no NaN / Inf
// over random parameter sets, the voice ends after its release, the
// framework never sees Volume applied, and every factory VAP patch
// ($OT_VENDOR_FIXTURES/mini-patches-2.0/VAP, skipped when absent) renders
// one second cleanly. Cycle counts: tests/audio_bench.cpp (vapinit /
// vapworst, make perf-gate).
#include "test.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio/engines/vap.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

using namespace ot;
using namespace ot::audio;
namespace fs = std::filesystem;

namespace {

constexpr int kN = vap::kParamCount;
constexpr int kBlocksPerSec = int(kSampleRate / float(kBlock)) + 1;   // 345

const PatchParamRange* range(int i) { return synthParamRange(kSynthEngineVap, i); }

// Default (Init) values.
std::vector<float> initValues() {
    std::vector<float> v(kN);
    for (int i = 0; i < kN; ++i) v[i] = range(i)->def;
    return v;
}

// One engine voice pool driven directly (no framework).
struct Rig {
    ISynthEngine* e = synthEngineFor(kSynthEngineVap);
    SynthSlotScratch scratch{};
    SynthParams p;
    SynthVoice v[kSynthMaxVoices];
    bool alive[kSynthMaxVoices] = {};
    std::vector<float> L, R;

    explicit Rig(const std::vector<float>& vals) {
        p.engine = kSynthEngineVap;
        p.count = kN;
        set(vals);
        e->slotReset(scratch);
        for (auto& x : v) {
            x = SynthVoice{};
            x.slotScratch = &scratch;
        }
    }
    void set(const std::vector<float>& vals) {
        for (int i = 0; i < kN; ++i) p.start[i] = p.end[i] = vals[i];
        ++p.serial;
    }
    void on(int k, float pitch, bool reused = false, bool overlap = false) {
        if (!reused) {
            std::memset(v[k].state, 0, sizeof(v[k].state));
            v[k].level = 0.0f;
        }
        v[k].gate = true;
        SynthNote n;
        n.pitch = pitch;
        n.note = uint8_t(pitch);
        n.reused = reused;
        n.overlap = overlap;
        e->noteOn(v[k], p, n);
        alive[k] = true;
    }
    void off(int k) {
        v[k].gate = false;
        e->noteOff(v[k], p);
    }
    // Render n blocks; appends to L / R. Returns the number of live voices.
    int run(int blocks, float gain = 1.0f) {
        int live = 0;
        for (int b = 0; b < blocks; ++b) {
            float l[kBlock] = {}, r[kBlock] = {};
            e->prepare(scratch, p);
            live = 0;
            for (int k = 0; k < kSynthMaxVoices; ++k) {
                if (!alive[k]) continue;
                alive[k] = e->render(v[k], p, gain, 0.0f, l, r);
                live += alive[k];
            }
            L.insert(L.end(), l, l + kBlock);
            R.insert(R.end(), r, r + kBlock);
        }
        return live;
    }
};

bool finite(const std::vector<float>& x) {
    for (float s : x)
        if (!std::isfinite(s)) return false;
    return true;
}
float peak(const std::vector<float>& x) {
    float m = 0.0f;
    for (float s : x) m = std::max(m, std::fabs(s));
    return m;
}
double rms(const std::vector<float>& x, size_t a = 0, size_t b = SIZE_MAX) {
    b = std::min(b, x.size());
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
    return b > a ? std::sqrt(s / double(b - a)) : 0.0;
}
double rmsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += double(a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(s / double(n));
}
// Fundamental (Hz) in [a, b) by autocorrelation over lags of 60..1500 Hz;
// the first lag within 90 % of the best one (no octave-down picks).
double pitchHz(const std::vector<float>& x, size_t a, size_t b) {
    const int lo = int(kSampleRate / 1500.0f), hi = int(kSampleRate / 60.0f);
    std::vector<double> ac(size_t(hi) + 2, 0.0);
    double best = 0.0;
    for (int lag = lo; lag <= hi + 1; ++lag) {
        double s = 0.0;
        for (size_t i = a; i + size_t(lag) < b; ++i) s += double(x[i]) * x[i + size_t(lag)];
        ac[size_t(lag)] = s;
        best = std::max(best, s);
    }
    for (int lag = lo + 1; lag <= hi; ++lag)
        if (ac[size_t(lag)] >= 0.9 * best && ac[size_t(lag)] >= ac[size_t(lag) - 1] && ac[size_t(lag)] >= ac[size_t(lag) + 1]) {
            // parabolic interpolation of the peak
            const double y0 = ac[size_t(lag) - 1], y1 = ac[size_t(lag)], y2 = ac[size_t(lag) + 1];
            const double den = y0 - 2.0 * y1 + y2;
            const double off = den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
            return kSampleRate / (double(lag) + off);
        }
    return 0.0;
}

// The context every parameter is heard in: both oscillators part pulse and
// apart in pitch, envelopes that move, filter and wave envelopes routed, all
// six mod slots live, and a note sequence exercising glide / legato /
// release: note A, B legato over A, release, C on the released voice.
std::vector<float> richContext() {
    std::vector<float> c = initValues();
    c[vap::kShape1] = 0.8f;
    c[vap::kShape2] = 0.7f;
    c[vap::kTune2] = 7.0f;
    c[vap::kDetune] = 10.0f;
    c[vap::kCutoff] = 1500.0f;
    c[vap::kResonance] = 0.6f;
    c[vap::kFilterEnvAmt] = 0.3f;
    c[vap::kAmpD] = 0.3f;
    c[vap::kAmpS] = 0.5f;
    c[vap::kAmpR] = 0.2f;
    c[vap::kEnv1A] = 0.05f;
    c[vap::kEnv1S] = 0.3f;
    c[vap::kEnv2A] = 0.1f;
    c[vap::kEnv2D] = 0.3f;
    c[vap::kEnv2S] = 0.2f;
    c[vap::kMixEnvAmt] = 0.6f;
    c[vap::kNoise] = 0.05f;
    c[vap::kLfo1Ratio] = 7.0f;
    c[vap::kLfo2Ratio] = 3.0f;
    c[vap::kLfo2Retrig] = 0.0f;   // LFO 2 free running
    c[vap::kGlideTime] = 0.1f;
    c[vap::kNoteTrack] = 0.5f;
    // Mod slots: (src, dest, amt)
    const float slots[6][3] = {{3, 3, 0.2f}, {4, 8, 0.05f}, {1, 0, 0.5f}, {2, 12, 0.4f}, {0, 7, 0.3f}, {3, 11, 0.2f}};
    for (int k = 0; k < 6; ++k)
        for (int j = 0; j < 3; ++j) c[vap::kModBase + 3 * k + j] = slots[k][j];
    return c;
}

std::vector<float> renderSequence(const std::vector<float>& vals) {
    Rig r(vals);
    r.run(20);   // free-running LFOs move before the first note
    r.on(0, 57.0f);
    r.run(kBlocksPerSec / 4);
    r.on(0, 64.0f, true, true);   // legato over the held note
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 8);
    r.on(0, 52.0f, true, false);  // the voice in its release
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 4);
    return r.L;
}

// Values of parameter i to compare: every enum value, else min / mid / max.
std::vector<float> probeValues(int i) {
    const PatchParamRange* g = range(i);
    const bool isEnum = i == vap::kFilterType || i == vap::kLfo1Wave || i == vap::kLfo2Wave || i == vap::kLfo1Retrig ||
                        i == vap::kLfo2Retrig || i == vap::kGlideMode || i == vap::kReset1 || i == vap::kReset2 ||
                        i == vap::kLfo1Sync || i == vap::kLfo2Sync || i == vap::kOscSync ||
                        (i >= vap::kModBase && (i - vap::kModBase) % 3 != 2);
    std::vector<float> out;
    if (isEnum) {
        for (int k = int(g->min); k <= int(g->max); ++k) out.push_back(float(k));
    } else {
        out = {g->min, 0.5f * (g->min + g->max), g->max};
    }
    return out;
}

}  // namespace

TEST(registry_returns_vap) {
    ISynthEngine* e = synthEngineFor(kSynthEngineVap);
    CHECK(e != nullptr);
    CHECK(e != synthTestEngine());
    CHECK_STR(e->name(), "VAP");
    CHECK_EQ(e->id(), int32_t(kSynthEngineVap));
    CHECK(synthParamCount(kSynthEngineVap) == kN);
}

TEST(init_patch_plays_the_note) {
    Rig r(initValues());
    r.on(0, 57.0f);   // A3 = 220 Hz
    r.run(kBlocksPerSec);
    CHECK(finite(r.L));
    CHECK(peak(r.L) > 0.05f);
    CHECK(peak(r.L) < 2.0f);
    CHECK(r.L == r.R);   // mono voice
    // Init: two saws through a 400 Hz low pass: the fundamental dominates.
    const double hz = pitchHz(r.L, size_t(kSampleRate * 0.2), size_t(kSampleRate * 0.4));
    CHECK(hz > 219.0 && hz < 221.0);
    if (!(hz > 219.0 && hz < 221.0)) std::printf("       crossing rate %.1f Hz\n", hz);
}

TEST(volume_is_never_applied) {
    std::vector<float> a = initValues(), b = initValues();
    b[vap::kVolume] = 0.25f;
    Rig ra(a), rb(b);
    ra.on(0, 60.0f);
    rb.on(0, 60.0f);
    ra.run(40);
    rb.run(40);
    CHECK(ra.L == rb.L);
}

TEST(every_parameter_has_an_effect) {
    const std::vector<float> ctx = richContext();
    for (int i = 0; i < kN; ++i) {
        if (i == vap::kVolume) continue;   // the framework's job
        const std::vector<float> vals = probeValues(i);
        std::vector<std::vector<float>> outs;
        for (float x : vals) {
            std::vector<float> c = ctx;
            c[i] = x;
            // PW is heard on the pulse; Osc 2's sync partner needs osc 1 audible.
            outs.push_back(renderSequence(c));
            CHECK(finite(outs.back()));
        }
        for (size_t k = 1; k < outs.size(); ++k) {
            const double d = rmsDiff(outs[k - 1], outs[k]);
            const bool ok = d > 1.0e-4;
            CHECK(ok);
            if (!ok) std::printf("       param %d: %g vs %g differ by %g only\n", i, vals[k - 1], vals[k], d);
        }
    }
}

TEST(polyphonic_chord_is_the_sum_of_its_voices) {
    std::vector<float> c = initValues();
    c[vap::kCutoff] = 3000.0f;
    const float notes[4] = {48.0f, 55.0f, 60.0f, 64.0f};
    Rig chord(c);
    for (int k = 0; k < 4; ++k) chord.on(k, notes[k]);
    CHECK_EQ(chord.run(100), 4);
    std::vector<float> sum(chord.L.size(), 0.0f);
    for (int k = 0; k < 4; ++k) {
        Rig one(c);
        // same per-voice rng (S&H, noise) as in the chord: advance the slot rng alike
        for (int j = 0; j < k; ++j) one.on(7, 30.0f);
        one.alive[7] = false;
        one.on(k, notes[k]);
        one.run(100);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += one.L[i];
    }
    CHECK(rmsDiff(sum, chord.L) < 1.0e-5);
    CHECK(rms(chord.L) > 0.05);
    // Polyphony through the framework's pool size: 8 voices at once stay finite.
    Rig eight(c);
    for (int k = 0; k < kSynthMaxVoices; ++k) eight.on(k, 40.0f + 5.0f * float(k));
    CHECK_EQ(eight.run(50), kSynthMaxVoices);
    CHECK(finite(eight.L));
}

TEST(glide_moves_the_pitch_over_the_glide_time) {
    std::vector<float> c = initValues();
    c[vap::kShape1] = c[vap::kShape2] = 0.0f;   // triangles: clean zero crossings
    c[vap::kCutoff] = 20000.0f;
    c[vap::kResonance] = 0.0f;
    c[vap::kFilterEnvAmt] = 0.0f;
    c[vap::kFilterType] = 4.0f;   // LP SVF 12
    c[vap::kGlideTime] = 0.3f;
    c[vap::kGlideMode] = 0.0f;    // Always
    const size_t w = size_t(kSampleRate * 0.03);
    for (int mode = 0; mode < 2; ++mode) {
        std::vector<float> cc = c;
        if (mode == 1) cc[vap::kGlideTime] = 0.0f;
        Rig r(cc);
        r.on(0, 45.0f);   // 110 Hz
        r.run(60);
        const size_t at = r.L.size();
        r.on(0, 69.0f, true, true);   // 440 Hz
        r.run(kBlocksPerSec);
        const size_t e0 = at + size_t(kSampleRate * 0.05);
        const double early = pitchHz(r.L, e0, e0 + w);
        const double late = pitchHz(r.L, at + size_t(kSampleRate * 0.6), at + size_t(kSampleRate * 0.6) + w);
        CHECK(late > 438.0 && late < 442.0);
        if (mode == 0) CHECK(early > 150.0 && early < 350.0);   // still on its way up
        else CHECK(early > 435.0);             // no glide: there at once
    }
    // Overlap mode: a note on a released voice does not glide.
    c[vap::kGlideMode] = 1.0f;
    Rig r(c);
    r.on(0, 45.0f);
    r.run(30);
    r.off(0);
    r.run(2);
    const size_t at = r.L.size();
    r.on(0, 69.0f, true, false);
    r.run(30);
    CHECK(pitchHz(r.L, at + 64, at + 64 + w) > 435.0);
}

TEST(voice_finishes_after_release) {
    Rig r(initValues());   // release 0.3 s
    r.on(0, 60.0f);
    r.run(50);
    r.off(0);
    int blocks = 0;
    while (r.run(1) && blocks < 4 * kBlocksPerSec) ++blocks;
    CHECK(blocks > kBlocksPerSec / 10);   // a real release
    CHECK(blocks < kBlocksPerSec);        // ... ending well within 1 s
    CHECK(r.v[0].level == 0.0f);
    // Percussive: sustain 0 ends without a note off.
    std::vector<float> c = initValues();
    c[vap::kAmpS] = 0.0f;
    c[vap::kAmpD] = 0.2f;
    Rig q(c);
    q.on(0, 60.0f);
    blocks = 0;
    while (q.run(1) && blocks < 4 * kBlocksPerSec) ++blocks;
    CHECK(blocks < kBlocksPerSec);
    // level reports the amp envelope for voice stealing
    Rig s(initValues());
    s.on(0, 60.0f);
    s.run(10);
    CHECK(s.v[0].level > 0.9f);
}

// USER_REPORTS_R2 item 3: one note played three times on a fresh voice
// (the previous one released to silence, as after Stop) renders the same.
// Returns the three 0..0.5 s renders.
static std::vector<std::vector<float>> threeFreshNotes(const std::vector<float>& vals) {
    Rig r(vals);
    std::vector<std::vector<float>> out;
    for (int k = 0; k < 3; ++k) {
        r.L.clear();
        r.on(0, 60.0f);   // fresh voice: the framework zeroes its state
        r.run(kBlocksPerSec / 2);
        out.push_back(r.L);
        r.off(0);
        while (r.run(1)) {
        }
        r.run(kBlocksPerSec / 4);   // "stop and wait"
    }
    return out;
}

TEST(reset_on_repeats_bit_exactly) {
    std::vector<float> c = initValues();
    c[vap::kReset1] = c[vap::kReset2] = 1.0f;
    const auto n = threeFreshNotes(c);
    CHECK(n[0] == n[1]);
    CHECK(n[0] == n[2]);
    CHECK(rms(n[0]) > 0.05);
}

TEST(init_patch_reset_off_repeats_bit_exactly) {
    const std::vector<float> c = initValues();
    CHECK(c[vap::kReset1] < 0.5f && c[vap::kReset2] < 0.5f);   // Init = Reset Off
    const auto n = threeFreshNotes(c);
    CHECK(n[0] == n[1]);
    CHECK(n[0] == n[2]);
    // No random relative phase between the two equal saws: a fresh Reset Off
    // voice starts like Reset On (both oscillators at 0), the full double saw.
    std::vector<float> on = c;
    on[vap::kReset1] = on[vap::kReset2] = 1.0f;
    CHECK(threeFreshNotes(on)[0] == n[0]);
    // The rich context (noise, a free-running LFO, detune) moves between the
    // notes, but the same history renders the same.
    CHECK(threeFreshNotes(richContext()) == threeFreshNotes(richContext()));
}

// The first two floats of vap.cpp's VoiceState: the oscillator phases.
struct PhaseProbe {
    float ph1, ph2;
};

TEST(reset_off_is_free_running) {
    // A retrigger on the sounding voice: Reset On restarts the waves, Reset
    // Off continues the phases from where the voice was.
    std::vector<float> off = initValues(), on = initValues();
    on[vap::kReset1] = on[vap::kReset2] = 1.0f;
    Rig a(off), b(on);
    a.on(0, 60.0f);
    b.on(0, 60.0f);
    a.run(37);
    b.run(37);
    const float ph1 = a.v[0].as<PhaseProbe>().ph1, ph2 = a.v[0].as<PhaseProbe>().ph2;
    CHECK(ph1 != 0.0f);
    a.on(0, 60.0f, true);
    b.on(0, 60.0f, true);
    CHECK_EQ(a.v[0].as<PhaseProbe>().ph1, ph1);
    CHECK_EQ(a.v[0].as<PhaseProbe>().ph2, ph2);
    CHECK_EQ(b.v[0].as<PhaseProbe>().ph1, 0.0f);
    CHECK_EQ(b.v[0].as<PhaseProbe>().ph2, 0.0f);
    const size_t at = a.L.size();
    a.run(20);
    b.run(20);
    CHECK(rmsDiff(std::vector<float>(a.L.begin() + long(at), a.L.end()),
                  std::vector<float>(b.L.begin() + long(at), b.L.end())) > 1.0e-3);
    // Deterministic given the same history.
    Rig c(off);
    c.on(0, 60.0f);
    c.run(37);
    c.on(0, 60.0f, true);
    c.run(20);
    CHECK(c.L == a.L);
}

TEST(random_parameter_sets_stay_finite) {
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) * (1.0f / 16777216.0f);
    };
    int bad = 0;
    float worst = 0.0f;
    for (int set = 0; set < 1000; ++set) {
        std::vector<float> a(kN), b(kN);
        for (int i = 0; i < kN; ++i) {
            const PatchParamRange* g = range(i);
            a[i] = g->min + rnd() * (g->max - g->min);
            b[i] = rnd() < 0.5f ? a[i] : g->min + rnd() * (g->max - g->min);
            if (rnd() < 0.15f) a[i] = rnd() < 0.5f ? g->min : g->max;   // extremes
        }
        // short times keep the run short and busy
        for (int t : {vap::kAmpA, vap::kAmpD, vap::kAmpR, vap::kEnv1A, vap::kEnv1D, vap::kEnv1R, vap::kEnv2A,
                      vap::kEnv2D, vap::kEnv2R})
            a[t] *= 0.05f, b[t] *= 0.05f;
        Rig r(a);
        r.on(0, float(int(rnd() * 127.0f)));
        r.on(1, float(int(rnd() * 127.0f)));
        r.run(8);
        // a ramp: start = a, end = b
        for (int i = 0; i < kN; ++i) r.p.end[i] = b[i];
        ++r.p.serial;
        r.p.moving = true;
        r.run(1);
        for (int i = 0; i < kN; ++i) r.p.start[i] = b[i];
        r.p.moving = false;
        r.on(0, float(int(rnd() * 127.0f)), true, true);
        r.run(8);
        r.off(0);
        r.off(1);
        r.run(8);
        const float pk = peak(r.L);
        if (!finite(r.L) || pk > 50.0f) {
            ++bad;
            std::printf("       set %d peak %g:", set, pk);
            for (int i = 0; i < kN; ++i) std::printf(" %d=%g/%g", i, a[i], b[i]);
            std::printf("\n");
        }
        if (std::isfinite(pk)) worst = std::max(worst, pk);
    }
    CHECK_EQ(bad, 0);
    std::printf("  random sets: worst peak %.2f\n", worst);
}

TEST(factory_patches_render_cleanly) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    fs::path dir = fs::path(root ? root : "") / "mini-patches-2.0" / "VAP";
    std::error_code ec;
    if (!root || !fs::is_directory(dir, ec)) {
        std::printf("  SKIP: no vendor VAP patches (OT_VENDOR_FIXTURES=%s)\n", root ? root : "unset");
        return;
    }
    std::vector<fs::path> files;
    for (auto& de : fs::directory_iterator(dir, ec))
        if (de.path().extension() == ".patch") files.push_back(de.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() > 50);
    int n = 0;
    float worst = 0.0f;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static SynthPatch patch;
        if (decodePatch(bytes.data(), bytes.size(), &patch) != PatchStatus::kOk || patch.engine != kSynthEngineVap) {
            CHECK(false);
            continue;
        }
        uint8_t mv[kSynthSeqMacros];
        for (int m = 0; m < kSynthSeqMacros; ++m) mv[m] = synthMacroDefault(patch.macros[m].bipolar);
        std::vector<float> vals(kN);
        CHECK_EQ(synthEffectiveParams(patch, mv, 1.0f, vals.data()), kN);
        Rig r(vals);
        r.on(0, 48.0f);
        r.on(1, 55.0f);
        r.run(kBlocksPerSec * 7 / 10);
        r.off(0);
        r.off(1);
        r.run(kBlocksPerSec * 3 / 10);
        const float pk = peak(r.L);
        const bool ok = finite(r.L) && pk < 8.0f && rms(r.L) > 1.0e-4;
        CHECK(ok);
        if (!ok) std::printf("       %s: peak %g rms %g\n", f.filename().string().c_str(), pk, rms(r.L));
        worst = std::max(worst, pk);
        ++n;
    }
    std::printf("  %d factory VAP patches rendered, loudest peak %.2f\n", n, worst);
}
