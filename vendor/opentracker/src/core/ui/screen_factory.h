// Factory test mode (spec 05 §2.12 "Factory test", entered from Config with
// the code of spec 06 §7.6 = interface event 11).
//
// A full-screen overlay owned by the Ui: while it runs, every button, pad and
// encoder event goes to it and the active module (Config) is suspended. Ten
// steps, in the stock order: screen, inputs, pad backlight, RAM, MIDI
// loopback, radio, battery, audio line/mic loopback recordings, USB/SD,
// finished. The per-step pass criteria are not in the spec; the ones here are
// OpenTracker's own (see the step comments in screen_factory.cpp). The test
// never writes the EEPROM / config image. Leaving it returns to Config.
#pragma once

#include <cstdint>

namespace ot {

class App;
class Ui;
class Gfx;

// One power-on check the device glue can report (the hardware selftest's
// boot report: clocks, SDRAM, display, codec, keypads, pad LEDs).
struct FactoryCheck {
    const char* name;
    bool ok;
};

class FactoryTest {
public:
    enum Step : uint8_t {
        kScreen = 0,
        kInputs,
        kPadLeds,
        kRam,
        kMidi,
        kRadio,
        kBattery,
        kAudio,
        kUsbSd,
        kFinished,
        kStepCount,
    };
    enum Result : uint8_t { kPending = 0, kPass, kFail, kSkip };

    // Physical buttons tested in the inputs step: ids 0..0x20 (spec 05
    // §2.1.1; 0x21/0x22 are not produced by either keypad map).
    static constexpr int kButtons = 33;
    static constexpr int kRamPasses = 10;            // spec 05 §2.12
    static constexpr uint32_t kShiftSkipMs = 1500;   // "hold Shift to skip"
    static constexpr uint32_t kRecordFrames = 44100; // 1 s per loopback file

    FactoryTest(Ui& ui, App& app) : ui_(ui), app_(app) {}

    void start(uint32_t now);
    bool active() const { return active_; }
    int step() const { return step_; }
    Result result(int s) const { return s >= 0 && s < kStepCount ? res_[s] : kPending; }
    static const char* stepName(int s);

    void onButton(uint8_t id, uint8_t state);
    void onPad(uint8_t pad, uint8_t state);
    void onEncoder(int d);
    void update(uint32_t now);
    void draw(Gfx& g) const;
    void padLeds(uint8_t* levels) const;

    // Inspection (tests).
    int inputsLeft() const;
    uint32_t ramErrors() const { return ramErr_; }
    int ramPass() const { return ramPass_; }

private:
    void enter(int s);
    void finish(Result r);
    void next();
    void exit();
    void runMidi();
    void runRadio();
    void runSd();
    void audioStep(uint32_t now);
    void ramStep();

    Ui& ui_;
    App& app_;
    bool active_ = false;
    int step_ = kScreen;
    Result res_[kStepCount] = {};
    uint32_t now_ = 0;
    uint32_t stepAt_ = 0;
    uint32_t doneAt_ = 0;   // auto steps: result shown, advance after a pause
    bool done_ = false;
    char detail_[64] = {};

    // Inputs.
    uint64_t btnSeen_ = 0;
    uint64_t padSeen_ = 0;
    bool encUp_ = false, encDown_ = false;
    bool shiftHeld_ = false;
    uint32_t shiftAt_ = 0;

    // RAM test over the free tail of the sample pool.
    uint16_t* ram_ = nullptr;
    uint32_t ramWords_ = 0;
    uint32_t ramPos_ = 0;
    int ramPass_ = 0;
    bool ramVerify_ = false;
    uint32_t ramErr_ = 0;

    // Audio loopback recordings.
    int audioPhase_ = 0;   // 0 line in, 1 mic, 2 done
    uint32_t audioAt_ = 0;
    int16_t* savedBuf_ = nullptr;
    uint32_t savedCap_ = 0;
    bool audioOk_[2] = {};
};

}  // namespace ot
