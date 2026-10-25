// HAL interface smoke tests: the interfaces can be implemented by fakes and
// the IDisplay helpers expand to the expected primitive sequence.
#include "test.h"

#include <string>
#include <vector>

#include "hal_api.h"

using namespace ot;

class RecDisplay : public IDisplay {
public:
    std::vector<std::string> log;
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override { log.push_back("dlStart"); }
    void clearColorRGB(uint8_t, uint8_t, uint8_t) override { log.push_back("clearColorRGB"); }
    void clear(bool, bool, bool) override { log.push_back("clear"); }
    void colorRGB(uint8_t, uint8_t, uint8_t) override { log.push_back("colorRGB"); }
    void colorA(uint8_t) override { log.push_back("colorA"); }
    void lineWidth(uint16_t) override { log.push_back("lineWidth"); }
    void pointSize(uint16_t) override { log.push_back("pointSize"); }
    void scissorXY(uint16_t, uint16_t) override {}
    void scissorSize(uint16_t, uint16_t) override {}
    void begin(Prim p) override { log.push_back("begin " + std::to_string(int(p))); }
    void end() override { log.push_back("end"); }
    void vertex2f(int32_t x, int32_t y) override {
        log.push_back("v " + std::to_string(x) + "," + std::to_string(y));
    }
    void vertex2ii(uint16_t, uint16_t, uint8_t, uint8_t) override {}
    void text(int16_t, int16_t, uint8_t, uint16_t, const char* s) override { log.push_back(std::string("text ") + s); }
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t n) override {
        log.push_back("number " + std::to_string(n));
    }
    void swap() override { log.push_back("swap"); }
};

TEST(display_helpers) {
    RecDisplay d;
    d.rect(1, 2, 30, 40);
    d.line(-1, 0, 5, 6);
    CHECK_EQ(d.log.size(), size_t(8));
    CHECK(d.log[0] == "begin 9");
    // Pixel-centre vertices (+8/16): crisp FT81x RECTS / LINES edges.
    CHECK(d.log[1] == "v 24,40");
    CHECK(d.log[2] == "v 488,648");
    CHECK(d.log[3] == "end");
    CHECK(d.log[4] == "begin 3");
    CHECK(d.log[5] == "v -8,8");
    CHECK_EQ(int(IDisplay::kOptCenter), int(IDisplay::kOptCenterX | IDisplay::kOptCenterY));
}

struct FakeAudio : IAudioOut {
    FillFn fn = nullptr;
    void* ctx = nullptr;
    void setCallback(FillFn f, void* c) override {
        fn = f;
        ctx = c;
    }
    bool start() override { return fn != nullptr; }
    void stop() override {}
    void pump(int16_t* buf) { fn(buf, kBlockFrames, ctx); }
};

static void fillRamp(int16_t* out, uint32_t frames, void* ctx) {
    int* calls = static_cast<int*>(ctx);
    ++*calls;
    for (uint32_t i = 0; i < frames; ++i) {
        out[2 * i] = int16_t(i);
        out[2 * i + 1] = int16_t(-int(i));
    }
}

TEST(audio_pull_callback) {
    FakeAudio a;
    CHECK(!a.start());
    int calls = 0;
    a.setCallback(&fillRamp, &calls);
    CHECK(a.start());
    int16_t buf[IAudioOut::kBlockFrames * IAudioOut::kChannels];
    a.pump(buf);
    CHECK_EQ(calls, 1);
    CHECK_EQ(buf[2 * 127], 127);
    CHECK_EQ(buf[2 * 127 + 1], -127);
    CHECK_EQ(IAudioOut::kSampleRate, 44100u);
    CHECK_EQ(IAudioOut::kBlockFrames, 128u);
}

TEST(input_event_defaults) {
    InputEvent e;
    CHECK(e.type == InputEvent::kNone);
    CHECK_EQ(e.delta, 0);
    CHECK(MidiPortId::kDin != MidiPortId::kUsb);
}
