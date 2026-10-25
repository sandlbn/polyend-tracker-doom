// make_demo: writes a ready-to-play demo project onto an SD-card folder.
//
//   make_demo <sd-root>
//
// Drives a headless App (src/core, unchanged) over the simulator's host-folder
// IStorage, so every file is written by the same code that loads it on the
// device: new project, two generated samples imported into instruments 1/2,
// a 64-step kick/hat pattern at 120 BPM, then the Workspace is saved and the
// project is also saved as Projects/OT Demo.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "app/app.h"
#include "app/pattern_ops.h"
#include "hal_api.h"
#include "host_storage.h"

namespace {

class NullDisplay : public ot::IDisplay {
public:
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
    void vertex2ii(uint16_t, uint16_t, uint8_t, uint8_t) override {}
    void text(int16_t, int16_t, uint8_t, uint16_t, const char*) override {}
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t) override {}
    void swap() override {}
};
class NullAudio : public ot::IAudioOut {
public:
    void setCallback(FillFn, void*) override {}
    bool start() override { return true; }
    void stop() override {}
};
class NullInput : public ot::IInput {
public:
    bool poll(ot::InputEvent&) override { return false; }
};
class NullLeds : public ot::ILeds {
public:
    uint8_t padCount() const override { return 48; }
    void setPad(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void clear() override {}
    void show() override {}
};
class NullMidi : public ot::IMidiPort {
public:
    bool send(ot::MidiPortId, const uint8_t*, uint32_t) override { return true; }
    uint32_t receive(ot::MidiPortId, uint8_t*, uint32_t) override { return 0; }
};
class FixedClock : public ot::IClock {
public:
    uint32_t ms = 1000;
    uint32_t micros() const override { return ms * 1000; }
    uint32_t millis() const override { return ms; }
};

constexpr int kRate = 44100;
constexpr double kPi = 3.14159265358979323846;

// Kick: sine sweeping 150 -> 45 Hz with an exponential amplitude decay.
std::vector<int16_t> makeKick() {
    const int n = kRate * 35 / 100;
    std::vector<int16_t> s(n);
    double phase = 0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const double f = 45.0 + 105.0 * std::exp(-t * 25.0);
        phase += 2 * kPi * f / kRate;
        double a = std::exp(-t * 9.0);
        if (i < 44) a *= i / 44.0;   // 1 ms attack, no click
        s[i] = int16_t(std::lround(std::sin(phase) * a * 30000.0));
    }
    return s;
}

// Hat: high-passed white noise burst with a fast decay.
std::vector<int16_t> makeHat() {
    const int n = kRate * 8 / 100;
    std::vector<int16_t> s(n);
    uint32_t r = 0x12345678u;
    double prev = 0;
    for (int i = 0; i < n; ++i) {
        r = r * 1664525u + 1013904223u;
        const double w = double(int32_t(r)) / 2147483648.0;
        const double hp = w - prev;   // first-difference high-pass
        prev = w;
        const double t = double(i) / kRate;
        s[i] = int16_t(std::lround(std::max(-1.0, std::min(1.0, hp * 0.6)) * std::exp(-t * 45.0) * 26000.0));
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_demo <sd-root>\n");
        return 2;
    }
    sim::HostStorage fs;
    if (!fs.init(argv[1])) {
        std::fprintf(stderr, "make_demo: cannot use SD root %s\n", argv[1]);
        return 1;
    }
    NullDisplay disp;
    NullAudio audio;
    NullInput input;
    NullLeds leds;
    NullMidi midi;
    FixedClock clock;

    std::vector<int16_t> bank(4u * 1024 * 1024), fx(ot::audio::AudioEngine::kFxMemorySamples);
    std::vector<ot::PatternStock> pats(ot::kPatternCount);
    std::vector<ot::Pattern> undo(20), fundo(10), seqBufs(2);
    std::vector<ot::Track> extTracks(ot::ExtTrackPool::kDefaultBlocks + 1);
    std::vector<uint8_t> extMeta(ot::ExtTrackPool::metaBytes(ot::ExtTrackPool::kDefaultBlocks));
    ot::Pattern clip;
    std::vector<uint8_t> nums(64), scratch(0x10000);
    std::unique_ptr<ot::App> app(new ot::App());
    ot::HalRefs h;
    h.display = &disp;
    h.audio = &audio;
    h.storage = &fs;
    h.midi = &midi;
    h.input = &input;
    h.leds = &leds;
    h.clock = &clock;
    ot::AppMemory m;
    m.sampleBank = bank.data();
    m.sampleBankSamples = uint32_t(bank.size());
    m.fxMemory = fx.data();
    m.fxSamples = uint32_t(fx.size());
    m.patterns = pats.data();
    m.seqBuffers = seqBufs.data();
    m.extTracks = extTracks.data();
    m.extTrackBlocks = int(extTracks.size());
    m.extMeta = extMeta.data();
    m.undo = undo.data();
    m.undoNums = nums.data();
    m.undoSlots = 20;
    m.fileUndo = fundo.data();
    m.fileUndoNums = nums.data() + 32;
    m.fileUndoSlots = 10;
    m.clipboard = &clip;
    m.scratch = scratch.data();
    m.scratchBytes = uint32_t(scratch.size());

    auto fail = [](const char* what) {
        std::fprintf(stderr, "make_demo: %s failed\n", what);
        return 1;
    };
    if (!app->init(h, m)) return fail("App::init");
    if (!app->boot()) return fail("App::boot");
    if (!app->newProject()) return fail("newProject");

    // Source samples on the card (written by the app's own WAV writer).
    fs.mkdir("Samples");
    fs.mkdir("Samples/OT Demo");
    const std::vector<int16_t> kick = makeKick(), hat = makeHat();
    if (!app->writeWav("Samples/OT Demo/kick.wav", kick.data(), uint32_t(kick.size()))) return fail("write kick.wav");
    if (!app->writeWav("Samples/OT Demo/hat.wav", hat.data(), uint32_t(hat.size()))) return fail("write hat.wav");
    if (!app->importSample("Samples/OT Demo/kick.wav", 0)) return fail("import kick");
    if (!app->importSample("Samples/OT Demo/hat.wav", 1)) return fail("import hat");

    // Pattern 1: 64 steps; track 1 kick every 4th step, track 2 hat on even steps.
    ot::Pattern& p = app->pattern();
    ot::pops::setLength(p, 64);
    const int8_t kC4 = 48;   // noteName(): octave = note / 12
    for (int s = 0; s < 64; ++s) {
        if (s % 4 == 0) {
            p.tracks[0].steps[s].note = kC4;
            p.tracks[0].steps[s].instrument = 0;   // instrument 1
        }
        if (s % 2 == 0) {
            p.tracks[1].steps[s].note = kC4;
            p.tracks[1].steps[s].instrument = 1;   // instrument 2
        }
    }
    app->patternEdited();
    app->project.setTempo(120.0f);
    app->setCurrentInstrument(0);
    app->markProjectDirty();
    app->applySequencerSettings();

    // Save As also autosaves the Workspace (now named "OT Demo").
    if (!app->saveProjectAs("OT Demo")) return fail("saveProjectAs");
    app->autosave();
    std::printf("make_demo: wrote demo project to %s (Workspace + Projects/OT Demo)\n", argv[1]);
    return 0;
}
