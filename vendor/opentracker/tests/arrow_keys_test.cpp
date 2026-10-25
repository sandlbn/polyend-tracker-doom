// Arrow keys and list direction per screen (user report on 2.0.1, also on
// 1.9.4): "instead of moving to adjacent boxes with the arrow keys I need the
// soft keys" and "a menu where Up and Down scroll were reversed".
// Table: screen x key -> expected focus / selection, against spec 06 and
// spec 05 appendix C.
#include <cstdio>
#include <functional>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_fields.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
template <class T>
T& scr(Harness& h, int mod) {
    return *static_cast<T*>(h.app->ui.screen(mod));
}

void addSample(Harness& h) {
    std::vector<int16_t> s(30000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t((i * 37) % 2000 - 1000);
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), uint32_t(s.size()), 1, out.data(), out.size()));
    h.fs.files["n.wav"] = out;
    CHECK(h.app->importSample("n.wav", 0));
    h.app->setCurrentInstrument(0);
}

constexpr int kEnc = -1;   // row key: encoder +1 instead of a button

struct Row {
    const char* what;
    std::function<void(Harness&)> setup;   // opens the screen and sets the start state
    int key;                                // button id, or kEnc
    std::function<int(Harness&)> get;
    int want;
};
}  // namespace

TEST(arrow_and_list_direction_table) {
    auto editorList = [](int effect) {
        return [effect](Harness& h) {
            addSample(h);
            h.tap(btn::SampleEditor);
            auto& e = scr<EditorScreen>(h, kModEditor);
            e.focus = 3;
            e.effect = effect;
        };
    };
    auto effect = [](Harness& h) { return scr<EditorScreen>(h, kModEditor).effect; };
    auto recorder = [](int sel) {
        return [sel](Harness& h) {
            h.tap(btn::SampleRecorder);
            scr<RecorderScreen>(h, kModRecorder).sel = sel;
        };
    };
    auto recSel = [](Harness& h) { return scr<RecorderScreen>(h, kModRecorder).sel; };
    auto master = [](int sub, int param) {
        return [sub, param](Harness& h) {
            h.tap(btn::Master);
            auto& m = scr<MixerScreen>(h, kModMaster);
            m.page = 0;
            m.sub = sub;
            m.param = param;
        };
    };
    auto masterParam = [](Harness& h) { return scr<MixerScreen>(h, kModMaster).param; };
    auto config = [](int cat) {
        return [cat](Harness& h) {
            h.tap(btn::Config);
            auto& c = scr<ConfigScreen>(h, kModConfig);
            c.focus = 0;
            c.category = cat;
        };
    };
    auto category = [](Harness& h) { return scr<ConfigScreen>(h, kModConfig).category; };
    auto playModeList = [](Harness& h) {
        addSample(h);
        h.tap(btn::SamplePlayback);
        scr<PlaybackScreen>(h, kModPlayback).sel = 6;
        h.app->instruments[h.app->currentInstrument()].playMode = 2;
    };
    auto playMode = [](Harness& h) { return int(h.app->instruments[h.app->currentInstrument()].playMode); };

    const std::vector<Row> rows = {
        // Sample Editor effect list (spec 06 §6.3): Up = previous, Down = next,
        // encoder + = next.
        {"editor list Up", editorList(5), btn::Up, effect, 4},
        {"editor list Down", editorList(5), btn::Down, effect, 6},
        {"editor list enc+", editorList(5), kEnc, effect, 6},
        // Sample Recorder setup (appendix C §2.13): Left / Right step through
        // Source, Level, Gain, Monitor (Radio Freq only with the radio source).
        {"recorder Right from Source", recorder(0), btn::Right, recSel, 4},
        {"recorder Right from Level", recorder(4), btn::Right, recSel, 5},
        {"recorder Right from Gain", recorder(5), btn::Right, recSel, 6},
        {"recorder Right at Monitor", recorder(6), btn::Right, recSel, 6},
        {"recorder Left from Level", recorder(4), btn::Left, recSel, 0},
        {"recorder Left from Monitor", recorder(6), btn::Left, recSel, 5},
        // Master 1/3 (spec 06 §4.2): Left / Right through Volume, Saturation,
        // Bass Boost, Space; sub-pages: the parameters (spec 06 §4.3).
        {"master Right from Volume", master(0, 0), btn::Right, masterParam, 1},
        {"master Right from Saturation", master(0, 1), btn::Right, masterParam, 2},
        {"master Left from Space", master(0, 3), btn::Left, masterParam, 2},
        {"reverb Right", master(1, 0), btn::Right, masterParam, 1},
        {"limiter Right", master(3, 1), btn::Right, masterParam, 2},
        {"eq Left", master(4, 2), btn::Left, masterParam, 1},
        // Config categories (spec 06 §7.4): Up / Down -+1, encoder += d.
        {"config Up", config(2), btn::Up, category, 1},
        {"config Down", config(2), btn::Down, category, 3},
        {"config enc+", config(2), kEnc, category, 3},
        // Sample Playback Play mode list (user report 2.0.5): drawn top to
        // bottom, Up = the mode above, Down = the mode below, encoder + = below.
        {"playback mode list Up", playModeList, btn::Up, playMode, 1},
        {"playback mode list Down", playModeList, btn::Down, playMode, 3},
        {"playback mode list enc+", playModeList, kEnc, playMode, 3},
    };
    for (const Row& r : rows) {
        Harness h;
        r.setup(h);
        h.run(20);
        if (r.key == kEnc) h.enc(1);
        else h.tap(uint8_t(r.key));
        const int got = r.get(h);
        if (got != r.want) std::fprintf(stderr, "  %s: got %d, want %d\n", r.what, got, r.want);
        CHECK_EQ(got, r.want);
    }
}

TEST(recorder_arrow_slots) {
    // Setup page with the radio source: Radio Freq (1) is a box.
    CHECK_EQ(RecorderScreen::arrowSel(0, 1, 0, true), 1);
    CHECK_EQ(RecorderScreen::arrowSel(1, 1, 0, true), 4);
    CHECK_EQ(RecorderScreen::arrowSel(4, -1, 0, true), 1);
    CHECK_EQ(RecorderScreen::arrowSel(4, -1, 0, false), 0);
    CHECK_EQ(RecorderScreen::arrowSel(2, 1, 0, false), 4);   // from a stale slot
    // Edit page: Start, End, Zoom only.
    CHECK_EQ(RecorderScreen::arrowSel(1, 1, 1, false), 2);
    CHECK_EQ(RecorderScreen::arrowSel(2, 1, 1, false), 3);
    CHECK_EQ(RecorderScreen::arrowSel(3, 1, 1, false), 3);
    CHECK_EQ(RecorderScreen::arrowSel(1, -1, 1, false), 1);
    CHECK_EQ(RecorderScreen::arrowSel(0, 1, 1, false), 1);
    CHECK_EQ(RecorderScreen::arrowSel(6, -1, 1, false), 3);
}

// Master pages: screen x parameter x key / encoder -> movement (owner test on
// stock 1.9.2: "Saturation Right -> Down, Left -> up | Equalizer right -> up,
// left -> down, all options lists are opposite"). Option lists, drawn top to
// bottom: encoder + (clockwise) and Down = the next item (one row down),
// encoder - and Up = the item above. Value bars: encoder + and Up = higher.
// Every page is covered with both kinds where it has both, so one rule can
// not be applied to the other kind. List values are the row from the top
// (Ping-pong / Sync: "On" row 0, "Off" row 1).
namespace {
int masterGet(Harness& h, int sub, int param) {
    App& a = *h.app;
    Project& p = a.project;
    const uint8_t fl = p.u8(pf::kDelayFlags);
    switch (sub * 10 + param) {
        case 0: return a.cfg.masterVolume;
        case 1: return p.u8(pf::kSaturation);
        case 2: return p.u8(pf::kBassBoost);
        case 3: return p.u8(pf::kSpace);
        case 10: return int(p.f32(pf::kReverbSize) * 100.0f + 0.5f);
        case 13: return int(p.f32(pf::kReverbSize + 12) * 1000.0f + 0.5f);
        case 14: return p.u8(pf::kLineInReverb);
        case 20: return (fl & 0x80) ? 0 : 1;
        case 21: return (fl & 0x40) ? 0 : 1;
        case 22: return fl & 0x3F;
        case 23: return p.u16(pf::kDelayTime);
        case 24: return p.u8(pf::kDelayFeedback);
        case 25: return p.u16(pf::kDelayFilter);
        case 26: return p.u8(pf::kLineInDelay);
        case 30: return p.u16(pf::kLimAttack);
        case 32: return p.u16(pf::kLimThreshold);
        case 33: return p.u8(pf::kSidechain);
        default: return int(p.f32(pf::kEqGain + 4 * uint32_t(param)) * 100.0f + (p.f32(pf::kEqGain + 4 * uint32_t(param)) < 0 ? -0.5f : 0.5f));
    }
}
void masterSet(Harness& h, int sub, int param, int v) {
    App& a = *h.app;
    Project& p = a.project;
    uint8_t fl = p.u8(pf::kDelayFlags);
    switch (sub * 10 + param) {
        case 0: a.cfg.masterVolume = uint8_t(v); break;
        case 1: p.setU8(pf::kSaturation, uint8_t(v)); break;
        case 2: p.setU8(pf::kBassBoost, uint8_t(v)); break;
        case 3: p.setU8(pf::kSpace, uint8_t(v)); break;
        case 10: p.setF32(pf::kReverbSize, v / 100.0f); break;
        case 13: p.setF32(pf::kReverbSize + 12, v / 1000.0f); break;
        case 14: p.setU8(pf::kLineInReverb, uint8_t(v)); break;
        case 20: p.setU8(pf::kDelayFlags, uint8_t(v == 0 ? fl | 0x80 : fl & ~0x80)); break;
        case 21: p.setU8(pf::kDelayFlags, uint8_t(v == 0 ? fl | 0x40 : fl & ~0x40)); break;
        case 22: p.setU8(pf::kDelayFlags, uint8_t((fl & 0xC0) | 0x40 | v)); break;   // Sync on: Rate active
        case 23: p.setU16(pf::kDelayTime, uint16_t(v)); break;
        case 24: p.setU8(pf::kDelayFeedback, uint8_t(v)); break;
        case 25: p.setU16(pf::kDelayFilter, uint16_t(v)); break;
        case 26: p.setU8(pf::kLineInDelay, uint8_t(v)); break;
        case 30: p.setU16(pf::kLimAttack, uint16_t(v)); break;
        case 32: p.setU16(pf::kLimThreshold, uint16_t(v)); break;
        case 33: p.setU8(pf::kSidechain, uint8_t(v)); break;
        default: p.setF32(pf::kEqGain + 4 * uint32_t(param), v / 100.0f); break;
    }
}
struct MasterRow {
    const char* what;
    int sub, param;
    bool list;      // MixerScreen::isListParam must agree
    int start;
    int key;        // btn::Up / btn::Down, or kEnc + 1 / kEnc - 1 (encoder +1 / -1)
    int want;
};
constexpr int kEncUp = 1000, kEncDown = 1001;   // encoder +1 (right) / -1 (left)
}  // namespace

TEST(master_list_and_bar_direction_table) {
    const std::vector<MasterRow> rows = {
        // Master 1/3: Volume / Bass Boost bars, Saturation list.
        {"master Volume enc+", 0, 0, false, 50, kEncUp, 51},
        {"master Volume enc-", 0, 0, false, 50, kEncDown, 49},
        {"master Volume Up", 0, 0, false, 50, btn::Up, 51},
        {"master Volume Down", 0, 0, false, 50, btn::Down, 49},
        {"master BassBoost enc+", 0, 2, false, 10, kEncUp, 11},
        {"master Saturation enc+ (down)", 0, 1, true, 2, kEncUp, 3},
        {"master Saturation enc- (up)", 0, 1, true, 2, kEncDown, 1},
        {"master Saturation Up", 0, 1, true, 2, btn::Up, 1},
        {"master Saturation Down", 0, 1, true, 2, btn::Down, 3},
        {"master Saturation Up at top", 0, 1, true, 0, btn::Up, 0},
        // Reverb: bars only.
        {"reverb Size enc+", 1, 0, false, 50, kEncUp, 51},
        {"reverb Size Up", 1, 0, false, 50, btn::Up, 51},
        {"reverb Size Down", 1, 0, false, 50, btn::Down, 49},
        {"reverb Diffusion enc-", 1, 3, false, 425, kEncDown, 417},
        {"reverb Line in enc+", 1, 4, false, 20, kEncUp, 21},
        // Delay: Ping-pong / Sync / Rate lists, Time .. Line in bars.
        {"delay PingPong enc+ On->Off", 2, 0, true, 0, kEncUp, 1},
        {"delay PingPong enc- Off->On", 2, 0, true, 1, kEncDown, 0},
        {"delay PingPong enc+ at Off", 2, 0, true, 1, kEncUp, 1},
        {"delay PingPong Up Off->On", 2, 0, true, 1, btn::Up, 0},
        {"delay PingPong Down On->Off", 2, 0, true, 0, btn::Down, 1},
        {"delay Sync enc+ On->Off", 2, 1, true, 0, kEncUp, 1},
        {"delay Sync Up Off->On", 2, 1, true, 1, btn::Up, 0},
        {"delay Rate enc+ (down)", 2, 2, true, 5, kEncUp, 6},
        {"delay Rate enc- (up)", 2, 2, true, 5, kEncDown, 4},
        {"delay Rate Up", 2, 2, true, 5, btn::Up, 4},
        {"delay Rate Down", 2, 2, true, 5, btn::Down, 6},
        {"delay Time enc+", 2, 3, false, 50, kEncUp, 51},
        {"delay Time Up", 2, 3, false, 50, btn::Up, 51},
        {"delay Feedback enc-", 2, 4, false, 30, kEncDown, 29},
        {"delay Filter Down", 2, 5, false, 10000, btn::Down, 9900},
        {"delay Line in enc+", 2, 6, false, 0, kEncUp, 1},
        // Limiter: Attack / Threshold bars, Sidechain list.
        {"limiter Attack enc+", 3, 0, false, 99, kEncUp, 100},
        {"limiter Attack Down", 3, 0, false, 99, btn::Down, 98},
        {"limiter Threshold enc+", 3, 2, false, 16383, kEncUp, 16383 + 327},
        {"limiter Sidechain enc+ (down)", 3, 3, true, 2, kEncUp, 3},
        {"limiter Sidechain enc- (up)", 3, 3, true, 2, kEncDown, 1},
        {"limiter Sidechain Up", 3, 3, true, 2, btn::Up, 1},
        {"limiter Sidechain Down", 3, 3, true, 2, btn::Down, 3},
        // EQ: bars only (gain x 100: one detent = 0.02).
        {"eq 115 Hz enc+", 4, 0, false, 0, kEncUp, 2},
        {"eq 115 Hz enc-", 4, 0, false, 0, kEncDown, -2},
        {"eq 990 Hz Up", 4, 2, false, 10, btn::Up, 12},
        {"eq 9900 Hz Down", 4, 4, false, 10, btn::Down, 8},
    };
    for (const MasterRow& r : rows) {
        Harness h;
        h.tap(btn::Master);
        auto& m = scr<MixerScreen>(h, kModMaster);
        m.page = 0;
        m.sub = r.sub;
        m.param = r.param;
        masterSet(h, r.sub, r.param, r.start);
        h.run(20);
        CHECK_EQ(MixerScreen::isListParam(r.sub, r.param), r.list);
        if (r.key == kEncUp) h.enc(1);
        else if (r.key == kEncDown) h.enc(-1);
        else h.tap(uint8_t(r.key));
        const int got = masterGet(h, r.sub, r.param);
        if (got != r.want) std::fprintf(stderr, "  %s: got %d, want %d\n", r.what, got, r.want);
        CHECK_EQ(got, r.want);
    }
}

TEST(mixer_fader_direction) {
    // Track Mixer 2/3 and Global Mixer 3/3: hold F1 + encoder / Up = higher.
    for (int page = 1; page <= 2; ++page)
        for (int key : {kEncUp, kEncDown, int(btn::Up), int(btn::Down)}) {
            Harness h;
            h.tap(btn::Master);
            auto& m = scr<MixerScreen>(h, kModMaster);
            m.page = page;
            const uint32_t off = page == 1 ? pf::kTrackVolume : pf::kMixDelay;
            h.app->project.setU8(off, 50);
            h.down(0);   // F1
            if (key == kEncUp) h.enc(1);
            else if (key == kEncDown) h.enc(-1);
            else h.tap(uint8_t(key));
            h.up(0);
            const bool upward = key == kEncUp || key == int(btn::Up);
            CHECK_EQ(int(h.app->project.u8(off)), upward ? 51 : 49);
        }
}
