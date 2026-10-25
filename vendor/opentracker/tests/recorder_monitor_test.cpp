// Recorder Monitor byte (project 0x139) with stock polarity, spec 22 §5:
// 0 = On, 1 = Off. A project saved by stock 1.9.2 must open with the same
// Monitor state and save the same byte back.
#include "test.h"
#include "ui_fakes.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "app/project_fields.h"
#include "formats/project_mt.h"

using namespace ot;

namespace {

std::vector<uint8_t> loadFixture(const char* name) {
    const char* dir = std::getenv("OT_FIXTURES");
    std::string path = std::string(dir ? dir : "fixtures") + "/" + name;
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

const char* const kFile = "Projects/Stock/project.mt";

// The stock project template with the recorder block as stock 1.9.2 stores
// it (spec 02 §3.3): source Radio, gains line 0x41 / mic LG 0x47 / mic HG
// 0x3C / radio 0x20, monitor byte `monitor`, 98.1 MHz.
bool putStockProject(Harness& h, uint8_t monitor) {
    std::vector<uint8_t> f = loadFixture("tl_project_template.mt");
    if (f.size() != mt::kFileSize) return false;
    const uint8_t rec[6] = {1, 0x41, 0x47, 0x3C, 0x20, monitor};
    for (int i = 0; i < 6; ++i) f[pf::kRecSource + i] = rec[i];
    const float mhz = 98.1f;
    std::memcpy(&f[pf::kRadioFreq], &mhz, 4);
    h.fs.dirs.insert("Projects");
    h.fs.dirs.insert("Projects/Stock");
    h.fs.files[kFile] = f;
    return true;
}

uint8_t savedByte(Harness& h, uint32_t off) {
    const std::vector<uint8_t>& f = h.fs.files[kFile];
    return f.size() > off ? f[off] : 0xEE;
}

}  // namespace

TEST(stock_project_monitor_on_opens_on_and_saves_zero) {
    Harness h;
    App& a = *h.app;
    CHECK(putStockProject(h, 0));            // stock: 0 = On
    CHECK(a.openProject("Projects", "Stock"));
    h.run(20);
    CHECK_EQ(a.cfg.recSource, 1);
    CHECK_EQ(a.cfg.recGain[1], 0x20);
    CHECK_EQ(a.cfg.recMonitor, 1);           // shown as "On"
    CHECK_EQ(a.cfg.radioFreq10, 981);
    // Saved unchanged: the recorder block is byte-for-byte what stock wrote.
    CHECK(a.saveProject());
    CHECK_EQ(savedByte(h, pf::kRecMonitor), 0);
    CHECK_EQ(savedByte(h, pf::kRecSource), 1);
    CHECK_EQ(savedByte(h, pf::kRecGainLine), 0x41);
    CHECK_EQ(savedByte(h, pf::kRecGainRadio), 0x20);
}

TEST(stock_project_monitor_off_opens_off_and_round_trips) {
    Harness h;
    App& a = *h.app;
    CHECK(putStockProject(h, 1));            // stock: 1 = Off
    CHECK(a.openProject("Projects", "Stock"));
    h.run(20);
    CHECK_EQ(a.cfg.recMonitor, 0);           // shown as "Off"
    CHECK(a.saveProject());
    CHECK_EQ(savedByte(h, pf::kRecMonitor), 1);
    // Monitor turned On in OpenTracker -> the stock byte 0.
    a.cfg.recMonitor = 1;
    h.run(20);
    CHECK_EQ(a.project.u8(pf::kRecMonitor), 0);
    CHECK(a.saveProject());
    CHECK_EQ(savedByte(h, pf::kRecMonitor), 0);
    // And reopened: On again.
    a.cfg.recMonitor = 0;
    h.run(20);
    CHECK(a.openProject("Projects", "Stock"));
    h.run(20);
    CHECK_EQ(a.cfg.recMonitor, 1);
}

TEST(recorder_screen_monitor_follows_the_stock_byte) {
    Harness h;
    App& a = *h.app;
    CHECK(putStockProject(h, 0));
    CHECK(a.openProject("Projects", "Stock"));
    h.run(20);
    a.cfg.recSource = 0;                     // line in (the host has no tuner)
    h.run(20);
    h.tap(btn::SampleRecorder);
    CHECK(!a.inputMonitorMuted);             // stock byte 0: monitored
    h.tap(btn::F7);
    h.tap(btn::F7);                          // Monitor -> Off
    CHECK(a.inputMonitorMuted);
    h.run(20);
    CHECK_EQ(a.project.u8(pf::kRecMonitor), 1);
}
