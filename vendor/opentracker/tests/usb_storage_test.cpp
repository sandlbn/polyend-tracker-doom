// 2.x USB Storage mode, app side (App::enterUsbStorage / exitUsbStorage, the
// Config > USB > USB Storage Off / On row, the mode page): transitions, the
// refusals, no card access while the computer owns it, the reload on exit and
// the never-persisted Off at boot. The USB protocol is msc_bot_test.cpp.
#include "test.h"
#include "ui_fakes.h"

#include "ui/screens.h"

using namespace ot;

namespace {

constexpr int kUsbCat = 3;   // Project Settings, General, MIDI, USB, ...

ConfigScreen& cfg(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }

// Config > USB > USB Storage with the keys: Enter opens the Off / On picker,
// Down / Up chooses, F8 applies.
void setRowWithKeys(Harness& h, bool on) {
    h.tap(btn::Config);
    ConfigScreen& c = cfg(h);
    for (int i = 0; i < 6 && c.category != kUsbCat; ++i) h.tap(c.category < kUsbCat ? btn::Down : btn::Up);
    CHECK_EQ(c.category, kUsbCat);
    h.tap(btn::Right);   // option list, "USB Storage"
    h.tap(btn::F8);      // Enter -> picker
    CHECK(c.editing);
    h.tap(on ? btn::Down : btn::Up);
    h.tap(btn::F8);      // Apply
}

int rowIndex(Harness& h) { return cfg(h).optionIndex(kUsbCat, 0); }

}  // namespace

TEST(usb_row_in_config) {
    Harness h;
    CHECK_STR(ConfigScreen::categoryName(kUsbCat), "USB");
    ConfigScreen::OptionInfo o;
    CHECK(ConfigScreen::optionInfo(kUsbCat, 0, &o));
    CHECK_STR(o.name, "USB Storage");
    CHECK_EQ(o.count, 2);
    CHECK(o.help != nullptr);
    char t[16];
    cfg(h).optionText(kUsbCat, 0, t, sizeof t);
    CHECK_STR(t, "Off");
}

TEST(toggle_on_off_on_exit) {
    Harness h;
    App& a = *h.app;
    // On from Config.
    setRowWithKeys(h, true);
    CHECK(a.usbStorageActive());
    CHECK(a.ui.usbStorageMode());
    CHECK_EQ(h.usb.begins, 1);
    CHECK(h.usb.active());
    CHECK_EQ(rowIndex(h), 1);
    h.run(40);
    CHECK(h.disp.lastHasText("USB Storage Mode"));
    CHECK(h.disp.lastHasText("Do not unplug"));
    CHECK(h.disp.lastHasText("Exit"));
    CHECK(h.usb.services > 0);   // the block I/O runs every loop pass
    // Off from Config: the same path as Exit (the page covers the Config
    // screen on the device; the row is applied through the screen's model).
    cfg(h).applyOption(kUsbCat, 0, 0);
    CHECK(!a.usbStorageActive());
    CHECK(!a.ui.usbStorageMode());
    CHECK_EQ(h.usb.ends, 1);
    CHECK(!h.usb.active());
    CHECK_EQ(rowIndex(h), 0);
    // On again, then Exit on the mode page.
    setRowWithKeys(h, true);
    CHECK(a.usbStorageActive());
    CHECK_EQ(h.usb.begins, 2);
    h.tap(btn::F8);   // Exit (computer has not used the drive)
    CHECK(!a.usbStorageActive());
    CHECK_EQ(h.usb.ends, 2);
    CHECK_EQ(rowIndex(h), 0);
    h.run(40);
    CHECK(!h.disp.lastHasText("USB Storage Mode"));
    CHECK_EQ(h.fs.lockedAccess, 0);
}

TEST(applying_on_twice_or_off_when_off_is_harmless) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    cfg(h).applyOption(kUsbCat, 0, 0);
    CHECK_EQ(h.usb.begins + h.usb.ends, 0);
    cfg(h).applyOption(kUsbCat, 0, 1);
    cfg(h).applyOption(kUsbCat, 0, 1);
    CHECK_EQ(h.usb.begins, 1);
    a.ui.setUsbStorageMode(false);
    CHECK_EQ(h.usb.ends, 1);
}

TEST(boot_is_always_off) {
    Harness h;
    App& a = *h.app;
    CHECK(!a.usbStorageActive());
    setRowWithKeys(h, true);
    CHECK(a.usbStorageActive());
    // Power cycle while the mode is on: a new App on the same card and
    // settings store comes up with the mode off and the card its own.
    Harness h2(false);
    h2.fs.files = h.fs.files;
    h2.fs.dirs = h.fs.dirs;
    h2.app->boot();
    CHECK(!h2.app->usbStorageActive());
    CHECK_EQ(h2.usb.begins, 0);
    char t[16];
    cfg(h2).optionText(kUsbCat, 0, t, sizeof t);
    CHECK_STR(t, "Off");
    // Config defaults / reset do not know the row either.
    Config c;
    c.setDefaults();
    CHECK(!h2.app->usbStorageActive());
}

TEST(eject_reminder_while_in_use) {
    Harness h;
    App& a = *h.app;
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
    h.usb.hostState = IUsbStorage::Host::kInUse;
    h.run(40);
    CHECK(h.disp.lastHasText("The computer is using the SD card."));
    h.tap(btn::F8);   // first Exit: reminder, still in the mode
    CHECK(a.usbStorageActive());
    CHECK(a.ui.usb.ejectWarningShown());
    h.run(40);
    CHECK(h.disp.lastHasText("Eject the drive on the computer first"));
    // The computer ejects: the reminder goes, Exit leaves at once.
    h.usb.hostState = IUsbStorage::Host::kEjected;
    h.run(40);
    CHECK(!a.ui.usb.ejectWarningShown());
    CHECK(h.disp.lastHasText("Ejected on the computer. Press Exit."));
    h.tap(btn::F8);
    CHECK(!a.usbStorageActive());
    // In use again: the second Exit leaves anyway.
    a.ui.setUsbStorageMode(true);
    h.usb.hostState = IUsbStorage::Host::kInUse;
    h.run(20);
    h.tap(btn::F8);
    CHECK(a.usbStorageActive());
    h.tap(btn::F8);
    CHECK(!a.usbStorageActive());
    CHECK_EQ(h.usb.ends, 2);
}

TEST(page_blocks_other_input) {
    Harness h;
    App& a = *h.app;
    a.ui.setUsbStorageMode(true);
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
    h.tap(btn::Song);
    h.pad(10);
    h.enc(3);
    h.tap(btn::F1);
    CHECK(a.usbStorageActive());
    // MIDI in is read and dropped (no transport start, no notes).
    a.cfg.transportIn = 1;
    a.applySequencerSettings();
    h.midi.in.push_back(0xFA);
    h.run(40);
    CHECK(!a.seq.isPlaying());
    CHECK(h.midi.in.empty());
}

TEST(enter_stops_playback_and_recording_and_saves) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    a.seq.setRecording(true);
    a.markProjectDirty();
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
    CHECK(!a.seq.isPlaying());
    CHECK(!a.seq.isRecording());
    CHECK(!a.capture.active());
    CHECK(!a.jobBusy());
    // Everything the app keeps on the card was written before the hand-over:
    // nothing is left for the autosave.
    CHECK(!a.startAutosave());
}

TEST(no_card_access_while_the_computer_owns_it) {
    Harness h;
    App& a = *h.app;
    a.markProjectDirty();
    a.ui.setUsbStorageMode(true);
    a.markProjectDirty();       // even a dirty project must wait
    a.requestConfigSave(true);  // and the config
    h.run(70000);               // > autosave interval, card probes, config saves
    CHECK(a.usbStorageActive());
    CHECK_EQ(h.fs.lockedAccess, 0);
    h.tap(btn::F8);
    CHECK(!a.usbStorageActive());
}

TEST(exit_reloads_the_workspace) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    a.ui.setUsbStorageMode(true);
    // The computer replaces the card contents: the Workspace project is gone.
    h.fs.files.erase("Workspace/project.mt");
    h.tap(btn::F8);
    CHECK(!a.usbStorageActive());
    // Reloaded like after a card change: a new Workspace project was made.
    CHECK(h.fs.files.count("Workspace/project.mt") == 1);
    CHECK_EQ(a.ui.active(), int(kModPattern));
    h.run(100);
    CHECK(!h.disp.lastHasText("USB Storage Mode"));
}

TEST(refused_while_a_job_runs) {
    Harness h;
    App& a = *h.app;
    struct Ctx {
        int left = 1000;
    } ctx;
    CHECK(a.startExternalJob(
        "Exporting", [](void* c, uint32_t& cost) -> int {
            cost += 0x40000;
            return --static_cast<Ctx*>(c)->left > 0 ? 1 : 0;
        },
        [](void*) { return 10; }, &ctx));
    CHECK(a.jobBusy());
    a.ui.setUsbStorageMode(true);
    CHECK(!a.usbStorageActive());
    CHECK(!a.ui.usbStorageMode());
    CHECK_EQ(h.usb.begins, 0);
    // Through the Config row as well; the row stays Off.
    h.tap(btn::Config);
    cfg(h).applyOption(kUsbCat, 0, 1);
    CHECK(!a.usbStorageActive());
    CHECK_EQ(rowIndex(h), 0);
    a.finishJob();
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
}

TEST(background_autosave_is_finished_not_refused) {
    Harness h;
    App& a = *h.app;
    a.markProjectDirty();
    CHECK(a.startAutosave());
    CHECK_EQ(int(a.jobOp()), int(JobOp::Autosave));
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
    CHECK(!a.jobBusy());
}

TEST(refused_while_games_borrow_the_sample_memory) {
    Harness h;
    App& a = *h.app;
    a.sampleBankLent = true;
    a.ui.setUsbStorageMode(true);
    CHECK(!a.usbStorageActive());
    a.sampleBankLent = false;
}

TEST(no_card_or_no_usb_support) {
    Harness h;
    App& a = *h.app;
    h.usb.failBegin = true;
    a.ui.setUsbStorageMode(true);
    CHECK(!a.usbStorageActive());
    h.run(40);
    CHECK(h.disp.lastHasText("No SD card"));
    CHECK(!h.fs.locked);
    // OT_USB_MSC=0 image: no IUsbStorage at all.
    h.usb.failBegin = false;
    a.setUsbStorage(nullptr);
    h.run(2100);
    a.ui.setUsbStorageMode(true);
    CHECK(!a.usbStorageActive());
    h.run(40);
    CHECK(h.disp.lastHasText("USB storage not available"));
}

TEST(page_renders_only_on_a_state_change) {
    // Review B #6: in the mode the main loop is the computer's block I/O; the
    // page is drawn when something on it changes, not on every pass.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
    h.run(300);
    const int s0 = h.disp.swaps;
    h.run(3000);   // nothing changes on the page
    CHECK_EQ(h.disp.swaps, s0);
    // Keys, pads and the encoder are taken by the page and change nothing.
    h.tap(btn::Play);
    h.tap(btn::F1);
    h.pad(10);
    h.enc(3);
    h.run(300);
    CHECK_EQ(h.disp.swaps, s0);
    h.usb.hostState = IUsbStorage::Host::kInUse;
    h.run(100);
    CHECK_EQ(h.disp.swaps, s0 + 1);
    CHECK(h.disp.lastHasText("The computer is using the SD card."));
    h.run(3000);
    CHECK_EQ(h.disp.swaps, s0 + 1);
    h.tap(btn::F8);   // Exit: eject reminder
    CHECK(a.ui.usb.ejectWarningShown());
    h.run(100);
    CHECK_EQ(h.disp.swaps, s0 + 2);
}
