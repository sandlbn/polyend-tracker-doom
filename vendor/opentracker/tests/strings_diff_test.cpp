// Stock-1.9.2 string-diff items (docs/STRINGS_DIFF_1.9.3.md items 1, 3, 5-9):
// Note popup F-key note window with Shift Oct-/Note-/Note+/Oct+, the USB
// Storage Mode page, list-loading / export / delete progress titles, the
// rec-arm page labels, FX picker letter prefix and short FX top-bar names.
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "model/fx.h"
#include "ui/notes.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
static SongScreen& song(Harness& h) { return *static_cast<SongScreen*>(h.app->ui.screen(kModSong)); }

static std::string nn(int n) {
    char b[4];
    noteName(int8_t(n), b);
    return b;
}

static void loadSaw(Harness& h) {
    std::vector<int16_t> s(8000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t((int(i) % 200 - 100) * 250);
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), uint32_t(s.size()), 1, w.data(), w.size()));
    h.fs.mkdir("Samples");
    h.fs.files["Samples/saw.wav"] = w;
    CHECK(h.app->importSample("Samples/saw.wav", 0));
    h.app->setCurrentInstrument(0);
}

TEST(note_popup_fkeys_play_pick_and_shift_window) {
    Harness h;
    App& a = *h.app;
    loadSaw(h);
    h.tap(btn::Rec);   // edit mode
    CHECK(a.editMode);
    h.down(btn::Note);
    h.run(600);        // > 500 ms: Note popup
    CHECK(pe(h).popup().isOpen());
    CHECK_EQ(pe(h).popup().kind(), 0);
    const int base = pe(h).noteWindowBase();
    CHECK_EQ(base, pe(h).popup().cursor());   // window starts at the pre-selected note
    for (int i = 0; i < 8; ++i) CHECK(nn(base + i) == a.ui.bar.label[i].top);
    CHECK(a.ui.bar.label[0].highlight);

    // F3 plays and picks base+2.
    h.down(btn::F3);
    CHECK_EQ(pe(h).popup().cursor(), base + 2);
    CHECK(a.auditionTrack(0x72) >= 0);
    CHECK(a.ui.bar.label[2].highlight);
    h.up(btn::F3);
    CHECK_EQ(a.auditionTrack(0x72), -1);

    // Shift re-labels F1/F2/F7/F8 and shifts the window.
    h.down(btn::Shift);
    CHECK_STR(a.ui.bar.label[0].top, "Oct-");
    CHECK_STR(a.ui.bar.label[1].top, "Note-");
    CHECK_STR(a.ui.bar.label[6].top, "Note+");
    CHECK_STR(a.ui.bar.label[7].top, "Oct+");
    CHECK(nn(base + 3) == a.ui.bar.label[3].top);   // F3..F6 stay notes
    h.tap(btn::F8);
    CHECK_EQ(pe(h).noteWindowBase(), base + 12);
    h.tap(btn::F7);
    CHECK_EQ(pe(h).noteWindowBase(), base + 13);
    h.tap(btn::F2);
    CHECK_EQ(pe(h).noteWindowBase(), base + 12);
    h.tap(btn::F1);
    CHECK_EQ(pe(h).noteWindowBase(), base);
    CHECK_EQ(pe(h).popup().cursor(), base + 2);   // shifting does not pick
    h.tap(btn::F8);
    h.up(btn::Shift);
    CHECK(nn(base + 12) == a.ui.bar.label[0].top);

    // F2 picks base+12+1; releasing Note commits it at the cursor.
    h.tap(btn::F2);
    CHECK_EQ(pe(h).popup().cursor(), base + 13);
    h.up(btn::Note);
    CHECK(!pe(h).popup().isOpen());
    CHECK_EQ(a.pattern().tracks[0].steps[0].note, base + 13);
}

TEST(note_popup_window_clamps) {
    Harness h;
    (void)h.app;
    h.tap(btn::Rec);
    h.down(btn::Note);
    h.run(600);
    h.down(btn::Shift);
    for (int i = 0; i < 12; ++i) h.tap(btn::F8);
    CHECK_EQ(pe(h).noteWindowBase(), 120);   // F8 = G-9 (127)
    for (int i = 0; i < 12; ++i) h.tap(btn::F1);
    CHECK_EQ(pe(h).noteWindowBase(), 0);
    h.up(btn::Shift);
    h.up(btn::Note);
    CHECK(!pe(h).popup().isOpen());
}

TEST(rec_arm_page_live_rec_labels) {
    Harness h;
    App& a = *h.app;
    h.down(btn::Shift);
    h.tap(btn::Rec);
    CHECK(pe(h).recArmPage);
    CHECK_STR(a.ui.bar.label[0].top, "Live REC");
    CHECK_STR(a.ui.bar.label[0].bottom, "Armed");
    h.tap(btn::F1);
    CHECK_STR(a.ui.bar.label[0].top, "Live REC");
    CHECK_STR(a.ui.bar.label[0].bottom, "Unarmed");
    h.up(btn::Shift);
}

TEST(fx_picker_letter_prefix_and_short_top_bar_names) {
    Harness h;
    App& a = *h.app;
    int vol = -1;
    for (int i = 0; i < kFxCount; ++i)
        if (kFxMenuOrder[i] == uint8_t(FxId::Volume)) vol = i;
    CHECK(vol >= 0 && vol < 48);
    h.down(btn::Fx1);
    h.run(300);   // > 180 ms: FX list
    CHECK(pe(h).popup().isOpen());
    CHECK_EQ(pe(h).popup().kind(), 3);
    h.pad(uint8_t(vol));
    h.tap(btn::F7);   // description window shows the full item text
    CHECK(pe(h).popup().descriptionOpen());
    CHECK(h.disp.lastHasText("V - Volume/Velocity"));
    h.tap(btn::F7);
    h.up(btn::Fx1);

    a.pattern().tracks[0].steps[0].fx[0].type = uint8_t(FxId::RandomNote);
    h.tap(btn::Fx1);   // FX column
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("Rnd Note"));
    a.pattern().tracks[0].steps[0].fx[0].type = uint8_t(FxId::MicroTune);
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("Micro-tune"));
    CHECK(!h.disp.lastHasText("Pitchbend"));
}

TEST(song_delete_shows_deleting_progress) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[0].note = 60;
    h.tap(btn::Song);
    SongScreen& s = song(h);
    s.edit = true;
    s.focus = 0;
    h.tap(btn::Delete);
    CHECK_STR(a.ui.progressTitle(), "Deleting");
    CHECK(!a.ui.progressOpen());
    CHECK_EQ(a.pattern().tracks[0].steps[0].note, kNoteEmpty);
}

TEST(list_loading_progress_titles) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::File);
    h.tap(btn::F3);   // Import Mod browser
    CHECK_STR(a.ui.progressTitle(), kLoadingFiles);
    CHECK(!a.ui.progressOpen());
    h.tap(btn::F7);   // Cancel
    h.tap(btn::F2);   // Open browser
    CHECK_STR(a.ui.progressTitle(), kLoadingProjects);
    CHECK(!a.ui.progressOpen());

    h.fs.dirs.insert("Firmware");
    h.fs.files["Firmware/PolyendTrackerOTv194.ptf"] = {1};
    h.tap(btn::Config);
    for (int i = 0; i < 5; ++i) h.tap(btn::Down);   // Firmware
    h.tap(btn::Right);
    h.tap(btn::F8);   // Firmware update
    CHECK_STR(a.ui.progressTitle(), kLoadingFirmwares);
    CHECK(!a.ui.progressOpen());
}

TEST(export_progress_titles_name_the_type) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[0].note = 60;
    char p[160];
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    CHECK(a.exportAudio(App::ExportKind::Pattern, p));
    CHECK_STR(a.ui.progressTitle(), kExportingPattern);
    CHECK(a.exportName(App::ExportKind::Song, p, sizeof(p)));
    a.exportAudio(App::ExportKind::Song, p);
    CHECK_STR(a.ui.progressTitle(), kExportingSong);
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p), true));
    a.exportAudio(App::ExportKind::Pattern, p, nullptr, true);
    CHECK_STR(a.ui.progressTitle(), kExportingPatternStems);
}

TEST(usb_storage_mode_page) {
    Harness h;
    App& a = *h.app;
    a.ui.setUsbStorageMode(true);
    h.run(40);
    CHECK(a.ui.usbStorageMode());
    CHECK(h.disp.lastHasText("USB Storage Mode"));
    CHECK(h.disp.lastHasText("Do not unplug"));
    CHECK(h.disp.lastHasText("Exit"));
    // Every other key, pad and the encoder is blocked.
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
    h.tap(btn::Song);
    CHECK_EQ(a.ui.active(), int(kModPattern));
    h.tap(btn::F1);
    CHECK(a.ui.usbStorageMode());
    // Exit (F8) closes it (2.x: the card comes back, usb_storage_test.cpp).
    h.tap(btn::F8);
    CHECK(!a.ui.usbStorageMode());
    h.run(40);
    CHECK(!h.disp.lastHasText("USB Storage Mode"));
    CHECK_EQ(a.ui.active(), int(kModPattern));
}
