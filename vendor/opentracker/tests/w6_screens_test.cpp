// 1.9.5 W6 screens in the stock chrome: display-list budget (FT81x RAM_DL
// is 2048 words) with the custom fonts bound, and a few layout facts the
// visual-diff scenes rely on (docs/VISUAL_DIFF_1.9.4.md §8.10-8.19).
#include "test.h"
#include "ui_fakes.h"

#include <cstdio>
#include <string>

#include "ui/gfx.h"
#include "ui/screens.h"

using namespace ot;

namespace {

struct Rig {
    Harness h{false};
    Rig() {
        h.disp.customFonts = true;
        // Enough SD entries for full, scrolling browser lists.
        h.fs.dirs.insert("Projects");
        for (int i = 0; i < 24; ++i) {
            char p[64];
            std::snprintf(p, sizeof(p), "Projects/Some Artist %02d - A Fairly Long Title", i);
            h.fs.dirs.insert(p);
            std::snprintf(p, sizeof(p), "A folder with a long name %02d", i);
            h.fs.dirs.insert(p);
        }
        h.app->boot();
    }
    App& a() { return *h.app; }
    uint32_t words() {
        a().ui.dirty();
        h.run(40);
        return a().ui.lastDlWords();
    }
};

void budget(Rig& r, const char* what) {
    const uint32_t w = r.words();
    std::printf("  DL words %-28s %u\n", what, unsigned(w));
    CHECK(w > 0);
    CHECK(w < 2048);
}

}  // namespace

TEST(w6_display_list_budget) {
    Rig r;
    CHECK(customFontsBound());
    App& a = r.a();
    r.h.tap(btn::Config);
    budget(r, "Config list");
    r.h.tap(btn::F2);
    r.h.tap(btn::F6);
    for (int i = 0; i < 6; ++i) r.h.tap(btn::F6);   // Recording options
    r.h.tap(btn::F8);
    CHECK(static_cast<ConfigScreen*>(a.ui.screen(kModConfig))->editing);
    budget(r, "Config picker + help");
    r.h.tap(btn::F7);
    r.h.tap(btn::F1);
    r.h.tap(btn::F1);   // Project Settings: Root note picker (37 values, scrollbar)
    r.h.tap(btn::F6);
    for (int i = 0; i < 3; ++i) r.h.tap(btn::F6);
    r.h.tap(btn::F8);
    budget(r, "Config picker (scroll)");
    r.h.tap(btn::F7);

    r.h.tap(btn::File);
    budget(r, "File menu");
    r.h.tap(btn::F2);
    budget(r, "File Open Project");
    r.h.tap(btn::F7);
    r.h.tap(btn::F6);
    budget(r, "File Export");
    r.h.tap(btn::F8);
    r.h.tap(btn::F3);
    budget(r, "File Import Mod");
    r.h.tap(btn::F7);

    r.h.tap(btn::SampleLoader);
    budget(r, "Sample Loader");

    r.h.tap(btn::Performance);
    budget(r, "Performance FX");

    r.h.tap(btn::Master);
    budget(r, "Master 1/3");
    for (uint8_t f : {btn::F2, btn::F3, btn::F4, btn::F5}) {
        r.h.tap(f);
        budget(r, "Master sub-page");
        r.h.tap(btn::F8);
    }
    r.h.tap(btn::Master);
    budget(r, "Track Mixer 2/3");
    r.h.tap(btn::Master);
    budget(r, "Global Mixer 3/3");
    applyFontSet(nullptr, 0);
}

// Config: values right-aligned at x 590 (ink ends 586..588), the wide
// "▲ item ▼" label also while the category list has the focus, and the
// 050505 strip with separators at 200 / 600 / 700 only [S snap 004, 030].
TEST(w6_config_chrome) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    CHECK_STR(a.ui.bar.label[0].top, "Project Settings");
    CHECK(a.ui.bar.label[0].wide);
    CHECK_STR(a.ui.bar.label[3].top, "Performance presets");
    CHECK(a.ui.bar.label[3].wide);
    CHECK_EQ(int(a.ui.bar.separatorMask()), 0xC4);
    CHECK_EQ(a.ui.bar.stripBg, 0x050505u);
    a.ui.dirty();
    h.run(40);
    bool right = false;
    for (const std::string& l : h.disp.last)
        if (l.rfind("TEXT 590 ", 0) == 0 && l.find("\"Global\"") != std::string::npos) right = true;
    CHECK(right);
}

// Mixer labels: fader level / action; the track name is the column header.
TEST(w6_mixer_labels) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::Master);
    CHECK_STR(a.ui.bar.label[0].bottom, "Mute");
    CHECK(std::string(a.ui.bar.label[0].top).find(" dB") != std::string::npos);
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("Track 1"));
}
