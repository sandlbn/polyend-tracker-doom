// Recorder monitor mix (spec 22 §5): with Monitor On the engine mixes the
// line input into the master at unity (stock 0x153B4(0x56), table[0x56] =
// 1.0), whatever the Global Mixer line-in level / mute. New projects have
// the Global Mixer line in muted (0x30A = 1), which made the monitor silent
// on the device when the mix used the Global Mixer gain.
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_fields.h"
#include "audio/audio_tables.h"
#include "audio/engine.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kAmp = 6000;   // below the new-project limiter (-6 dBFS)

// 1 kHz-ish sine RX signal, both channels.
void rxSine(int16_t* b, uint64_t blk) {
    for (int i = 0; i < 128; ++i) {
        const float ph = 2.0f * kPi * 1000.0f * float(blk * 128 + uint64_t(i)) / 44100.0f;
        const int16_t v = int16_t(float(kAmp) * std::sin(ph));
        b[2 * i] = v;
        b[2 * i + 1] = v;
    }
}

}  // namespace

TEST(recorder_monitor_on_idle_engine_hears_input_at_unity) {
    Harness h;
    App& a = *h.app;
    // The device case: a new project, Global Mixer line in muted.
    CHECK(a.project.u8(pf::kLineInMute) != 0);
    h.lineGen = rxSine;
    // Outside the Recorder the Global Mixer mute applies: silent.
    h.run(20);
    h.audio.pump(10);
    CHECK_EQ(h.audio.pump(20), 0);
    h.tap(btn::SampleRecorder);
    CHECK_EQ(a.cfg.recMonitor, 1);   // On
    CHECK(!a.inputMonitorMuted);
    h.run(20);
    h.audio.pump(10);   // ramps settle
    const int pk = h.audio.pump(40);
    // Unity monitor gain, then the master volume (Config, TRK law).
    const float want = float(kAmp) * trkGain(a.cfg.masterVolume);
    CHECK(want > 1000.0f);
    CHECK_NEAR(double(pk), double(want), double(want) * 0.03 + 2.0);
    // The diag peak is the unity mix (before the master volume).
    CHECK_NEAR(double(a.engine.takeLineMonitorPeak()), double(kAmp), 3.0);
    // Monitor Off -> silent.
    h.tap(btn::F7);
    h.tap(btn::F7);
    CHECK(a.inputMonitorMuted);
    h.audio.pump(10);
    CHECK_EQ(h.audio.pump(20), 0);
    CHECK_EQ(a.engine.takeLineMonitorPeak(), 0);
    // On again, then leaving the Recorder: Global Mixer (muted) again.
    h.tap(btn::F7);   // already selected: one press toggles
    CHECK(!a.inputMonitorMuted);
    h.audio.pump(10);
    CHECK(h.audio.pump(20) > 1000);
    h.tap(btn::Pattern);
    h.audio.pump(10);
    CHECK_EQ(h.audio.pump(20), 0);
}

TEST(recorder_monitor_on_ignores_global_mixer_line_level) {
    Harness h;
    App& a = *h.app;
    a.project.setU8(pf::kLineInMute, 0);
    a.project.setU8(pf::kLineInLevel, 0);   // fader at 0: silent outside the Recorder
    a.markProjectDirty();
    h.lineGen = rxSine;
    h.run(20);
    h.audio.pump(10);
    CHECK_EQ(h.audio.pump(20), 0);
    h.tap(btn::SampleRecorder);
    h.run(20);
    h.audio.pump(10);
    const float want = float(kAmp) * trkGain(a.cfg.masterVolume);
    CHECK_NEAR(double(h.audio.pump(40)), double(want), double(want) * 0.03 + 2.0);
}

namespace {

struct Rig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(1u << 20);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    Rig() {
        CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
        std::vector<int16_t> d(40000);
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = int16_t(5000.0f * std::sin(2.0f * kPi * 220.0f * float(i) / 44100.0f));
        ins[0].lengthFrames = uint32_t(d.size());
        ins[0].playMode = uint8_t(PlayMode::ForwardLoop);
        CHECK(eng->bank().store(0, d.data(), uint32_t(d.size()), false));
        eng->settings().lineInMute = true;   // new-project Global Mixer
        VoiceNoteOn n;
        n.instrument = 0;
        n.note = 60;
        eng->voiceNoteOn(0, n);
    }
};

}  // namespace

TEST(recorder_monitor_mixes_with_playing_voices) {
    Rig voiceOnly, mixed;
    int16_t a[256], b[256], line[256];
    int diffPk = 0, linePk = 0, voicePk = 0;
    for (uint64_t blk = 0; blk < 60; ++blk) {
        rxSine(line, blk);
        voiceOnly.eng->setLineMonitor(true);
        voiceOnly.eng->renderLive(a);   // monitor On, no input this block
        mixed.eng->setLineMonitor(true);
        mixed.eng->setLineInput(line);
        mixed.eng->renderLive(b);
        if (blk < 4) continue;   // ramps
        for (int i = 0; i < 256; ++i) {
            // The output lags the input (limiter look-ahead), so compare
            // levels: mixed - voices = the line at unity.
            const int d = std::abs(int(b[i]) - int(a[i]));
            diffPk = d > diffPk ? d : diffPk;
            voicePk = std::abs(a[i]) > voicePk ? std::abs(a[i]) : voicePk;
            linePk = std::abs(line[i]) > linePk ? std::abs(line[i]) : linePk;
        }
    }
    CHECK(voicePk > 1000);   // the voice plays
    CHECK(linePk > 5000);
    // mixed = voices + line at unity (master 86 = 0 dB, limiter open).
    CHECK_NEAR(double(diffPk), double(linePk), 3.0);
    // Monitor not set: the Global Mixer mute keeps the line out (voice
    // render identical with and without the line input).
    int err = 0;
    Rig r1, r2;
    for (uint64_t blk = 0; blk < 20; ++blk) {
        rxSine(line, blk);
        r1.eng->renderLive(a);
        r2.eng->setLineInput(line);
        r2.eng->renderLive(b);
        for (int i = 0; i < 256; ++i) err = std::abs(a[i] - b[i]) > err ? std::abs(a[i] - b[i]) : err;
    }
    CHECK_EQ(err, 0);
}
