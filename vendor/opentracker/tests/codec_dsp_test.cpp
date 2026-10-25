// Master EQ / Bass Boost / Space on the SGTL5000 DAP (spec 18 §1): register
// values and write sequences against a fake I2C register map, plus the app
// wiring (project values pushed to IAudioOut::applyMasterDsp).
#include "test.h"
#include "ui_fakes.h"

#include <map>
#include <utility>
#include <vector>

#include "app/project_fields.h"
#include "hal/audio_sgtl5000_dap.h"

using namespace ot;
using namespace ot::hal::sgtl_dap;

namespace {

// Fake codec register file. Seeded with the SGTL5000 datasheet reset values
// of the DAP registers (UNVERIFIED [D]: from the datasheet, not read from a
// device) and SSS_CTRL as left by the spec 01 §5.1 enable sequence.
struct FakeCodec {
    std::map<uint16_t, uint16_t> reg;
    std::vector<std::pair<uint16_t, uint16_t>> writes;
    FakeCodec() {
        reg[0x000A] = 0x0010;
        reg[0x0100] = 0x0000;
        reg[0x0104] = 0x0040;
        reg[0x0106] = 0x051F;
        reg[0x0108] = 0x0000;
        reg[0x010A] = 0x0040;
        for (int b = 0; b < 5; ++b) reg[uint16_t(0x116 + 2 * b)] = 0x002F;
    }
    Bus bus() {
        Bus b;
        b.read = [](void* c, uint16_t r, uint16_t& v) {
            v = static_cast<FakeCodec*>(c)->reg[r];
            return true;
        };
        b.write = [](void* c, uint16_t r, uint16_t v) {
            FakeCodec* f = static_cast<FakeCodec*>(c);
            f->reg[r] = v;
            f->writes.push_back({r, v});
            return true;
        };
        b.ctx = this;
        return b;
    }
};

using W = std::vector<std::pair<uint16_t, uint16_t>>;

bool same(const W& a, const W& b) {
    if (a.size() != b.size()) {
        std::printf("  write count %zu, expected %zu\n", a.size(), b.size());
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            std::printf("  write %zu: 0x%03X=0x%04X, expected 0x%03X=0x%04X\n", i, a[i].first, a[i].second,
                        b[i].first, b[i].second);
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(eq_band_codes) {
    CHECK_EQ(eqBandCode(0.0f), 0x2F);      // 0 dB
    CHECK_EQ(eqBandCode(1.0f), 0x5F);      // +12 dB
    CHECK_EQ(eqBandCode(-1.0f), 0x00);     // -11.75 dB
    CHECK_EQ(eqBandCode(2.0f), 0x5F);      // clamped
    CHECK_EQ(eqBandCode(-3.0f), 0x00);
    CHECK_EQ(eqBandCode(0.02f), 0x30);     // one detent: 0.96 + 0.499 -> 1
    CHECK_EQ(eqBandCode(-0.02f), 0x2E);    // -0.96 + 0.499 = -0.461 + 47 -> 46
    CHECK_EQ(eqBandCode(0.5f), 0x47);      // 24.499 + 47 -> 71
    CHECK_EQ(eqBandCode(-0.5f), 0x17);     // -23.501 + 47 -> 23
}

TEST(bass_ctrl_values) {
    // spec 18 §1.3: LR bits 13:8 = 11; BASS bits 6:0 = 127 at v=0, 25 at v=100.
    CHECK_EQ(bassCtrlValue(kBassLr, bassLevel(0)), 0x0B7F);
    CHECK_EQ(bassCtrlValue(kBassLr, bassLevel(100)), 0x0B19);
    CHECK_EQ(bassCtrlValue(kBassLr, bassLevel(200)), 0x0B19);   // v > 100 -> 0.8
    // Both float evaluation orders of 0.8*v/100 give the same register value.
    for (int v = 0; v <= 100; ++v) {
        const float alt = 0.8f * (float(v) / 100.0f);
        CHECK_EQ(bassCtrlValue(kBassLr, bassLevel(uint8_t(v))), bassCtrlValue(kBassLr, alt));
    }
}

TEST(init_sequence) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    CHECK(d.init());
    const W expect = {
        {0x0100, 0x0001},   // DAP_CONTROL: DAP on
        {0x000A, 0x0070},   // SSS_CTRL: I2S_IN -> DAP -> DAC
        {0x0108, 0x0003},   // eqSelect(3): 5-band GEQ
        {0x0116, 0x002F}, {0x0118, 0x002F}, {0x011A, 0x002F}, {0x011C, 0x002F}, {0x011E, 0x002F},
        {0x010A, 0x0040},   // surround off (bits 1:0 = 0)
        {0x0104, 0x0041},   // bass enhance EN
        {0x0104, 0x0101},   // BYPASS_HPF = 1, CUTOFF = 0
        {0x0106, 0x0B7F},   // LR 11, BASS 127
    };
    CHECK(same(c.writes, expect));
}

TEST(disable_order) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    d.disable();
    CHECK(same(c.writes, W{{0x000A, 0x0010}, {0x0100, 0x0000}}));
}

TEST(eq_mode_reselected) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    c.reg[0x0108] = 0x0001;   // PEQ mode
    const float g[5] = {1.0f, -1.0f, 0.0f, 0.5f, -0.5f};
    d.setEqBands(g);
    CHECK(same(c.writes, W{{0x0108, 0x0003}, {0x0116, 0x5F}, {0x0118, 0x00}, {0x011A, 0x2F},
                           {0x011C, 0x47}, {0x011E, 0x17}}));
    // Only the low 7 bits of a band register change.
    c.writes.clear();
    c.reg[0x0116] = 0xFF80;
    const float z[5] = {0, 0, 0, 0, 0};
    d.setEqBands(z);
    CHECK_EQ(c.reg[0x0116], 0xFFAF);
    CHECK_EQ(c.writes.size(), size_t(5));   // mode already 3: no 0x108 write
}

TEST(space_sequence) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    d.init();
    c.writes.clear();
    d.setSpace(1);   // enable first (flag set), then WIDTH 7, SELECT 3
    CHECK(same(c.writes, W{{0x010A, 0x0043}, {0x010A, 0x0073}}));
    c.writes.clear();
    d.setSpace(8);   // already enabled: WIDTH 0
    CHECK(same(c.writes, W{{0x010A, 0x0003}}));
    c.writes.clear();
    d.setSpace(0);   // off
    CHECK(same(c.writes, W{{0x010A, 0x0000}}));
    c.writes.clear();
    d.setSpace(4);   // re-enable, WIDTH 4
    CHECK(same(c.writes, W{{0x010A, 0x0003}, {0x010A, 0x0043}}));
}

TEST(bass_boost_keeps_enable) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    d.init();
    c.writes.clear();
    d.setBassBoost(0);   // level minimum, enhancer still enabled
    CHECK(same(c.writes, W{{0x0104, 0x0101}, {0x0106, 0x0B7F}}));
    c.writes.clear();
    d.setBassBoost(100);
    CHECK(same(c.writes, W{{0x0104, 0x0101}, {0x0106, 0x0B19}}));
    CHECK_EQ(c.reg[0x0104] & 1, 1);
}

TEST(apply_writes_only_changes) {
    FakeCodec c;
    Dap d;
    d.attach(c.bus());
    d.init();
    c.writes.clear();
    float eq[5] = {0, 0, 0, 0, 0};
    d.apply(eq, 0, 0);   // first call after init: every control
    CHECK(same(c.writes, W{{0x0116, 0x2F}, {0x0118, 0x2F}, {0x011A, 0x2F}, {0x011C, 0x2F}, {0x011E, 0x2F},
                           {0x0104, 0x0101}, {0x0106, 0x0B7F}, {0x010A, 0x0040}}));
    c.writes.clear();
    d.apply(eq, 0, 0);
    CHECK(c.writes.empty());
    d.apply(eq, 50, 0);
    CHECK(same(c.writes, W{{0x0104, 0x0101}, {0x0106, uint16_t(0x0B00 | (127 - 51))}}));
    c.writes.clear();
    eq[2] = 0.02f;
    d.apply(eq, 50, 0);
    CHECK(same(c.writes, W{{0x0116, 0x2F}, {0x0118, 0x2F}, {0x011A, 0x30}, {0x011C, 0x2F}, {0x011E, 0x2F}}));
    c.writes.clear();
    d.apply(eq, 50, 2);
    CHECK(same(c.writes, W{{0x010A, 0x0043}, {0x010A, 0x0063}}));
}

TEST(app_pushes_project_values) {
    Harness h(true, 256 * 1024);
    App& a = *h.app;
    h.run();
    CHECK(h.audio.dspCalls > 0);
    CHECK_EQ(h.audio.dspBass, a.project.u8(pf::kBassBoost));
    CHECK_EQ(h.audio.dspSpace, a.project.u8(pf::kSpace));
    a.project.setF32(pf::kEqGain + 4 * 3, 0.5f);
    a.project.setU8(pf::kBassBoost, 42);
    a.project.setU8(pf::kSpace, 6);
    h.run();
    CHECK(h.audio.dspEq[3] == 0.5f);
    CHECK_EQ(h.audio.dspBass, 42);
    CHECK_EQ(h.audio.dspSpace, 6);
}
