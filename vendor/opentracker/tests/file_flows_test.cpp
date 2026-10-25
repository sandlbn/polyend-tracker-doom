// File screen flows driven by key events only, as on the device (1.9.5 field
// report: Open Project "stuck", New Folder "does nothing", no stop prompt
// while playing). Spec 05 appendix E1.1-E1.4, E6. Every frame is checked
// against the FT81x RAM_DL limit (2048 words) with the custom fonts bound and
// a full SD list, like the owner's card: a frame over it faults the
// coprocessor and the screen keeps the old picture.
#include <cstdio>
#include <cstring>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_io.h"
#include "formats/file_io.h"
#include "formats/project_mt.h"
#include "ui/gfx.h"
#include "ui/screens.h"

using namespace ot;

namespace {

void addProject(Harness& h, const std::string& d) {
    for (size_t p = d.find('/'); p != std::string::npos; p = d.find('/', p + 1)) h.fs.dirs.insert(d.substr(0, p));
    h.fs.dirs.insert(d);
    Project p;
    p.setDefault();
    CHECK(saveProjectFile(h.fs, (d + "/project.mt").c_str(), p) == FmtStatus::Ok);
}

struct Rig {
    Harness h{false};
    uint32_t maxWords = 0;
    Rig() {
        h.disp.customFonts = true;
        // Sorted SD list: "A folder" (folder), "B project", then many projects.
        h.fs.dirs.insert("Projects");
        h.fs.dirs.insert("Projects/A folder");
        addProject(h, "Projects/A folder/Inner project");
        addProject(h, "Projects/B project");
        for (int i = 0; i < 24; ++i) {
            char p[80];
            std::snprintf(p, sizeof(p), "Projects/Some Artist %02d - A Fairly Long Title", i);
            addProject(h, p);
        }
        h.app->boot();
        CHECK(customFontsBound());
    }
    ~Rig() { applyFontSet(nullptr, 0); }
    App& a() { return *h.app; }
    FileScreen& fs() { return *static_cast<FileScreen*>(h.app->ui.screen(kModFile)); }
    // Draw now and keep the largest display list.
    void frame() {
        a().ui.dirty();
        h.run(30);
        const uint32_t w = a().ui.lastDlWords();
        if (w > maxWords) maxWords = w;
    }
    void tap(uint8_t id) {
        h.tap(id);
        frame();
    }
    void play() {
        tap(btn::Play);
        CHECK(a().seq.isPlaying());
    }
    void openBrowser() {
        tap(btn::File);
        tap(btn::F2);
        CHECK(fs().state == 1);
        CHECK(fs().focus == 0);
    }
    // Moves the SD cursor to `name` with the encoder (as on the device).
    void select(const char* name) {
        for (int i = 0; i < 40 && std::strcmp(fs().entries[fs().cursor], name) != 0; ++i) {
            h.enc(1);
            frame();
        }
        CHECK(std::strcmp(fs().entries[fs().cursor], name) == 0);
    }
    void waitJob() {
        for (int i = 0; i < 100 && a().ui.active() == kModFile; ++i) h.run(20);
    }
    // Gfx counts the screen's words; the HAL adds the per-frame font setup
    // (6 handles x 6 words) and DISPLAY, and longer names add glyphs: keep
    // headroom under the 2048-word RAM_DL (the keyboard frame over the full
    // browser measured 2010 + 37 before 1.9.5's title-only keyboard frame).
    void budget() {
        std::printf("  max DL words %u\n", unsigned(maxWords));
        CHECK(maxWords > 0 && maxWords <= 1900);
    }
};

}  // namespace

TEST(file_open_from_list_by_keys) {
    Rig r;
    r.openBrowser();
    r.select("B project");
    r.tap(btn::F8);
    r.waitJob();
    CHECK(r.a().ui.active() == kModPattern);
    CHECK(std::strcmp(r.a().project.name, "B project") == 0);
    CHECK(std::strcmp(r.a().project.folder, "Projects") == 0);
    r.budget();
}

// The owner's session: playing, into a folder, open a project in it.
TEST(file_open_from_subfolder_while_playing) {
    Rig r;
    r.play();
    r.openBrowser();
    r.select("A folder");
    r.tap(btn::F8);   // a folder: Open does nothing
    CHECK(r.a().ui.active() == kModFile);
    CHECK(!r.a().ui.askOpen());
    r.tap(btn::F1);   // Enter
    CHECK(r.fs().count == 2);   // "..", "Inner project"
    r.tap(btn::Down);
    r.tap(btn::EncPush);   // id 0x21 opens like F8
    CHECK(r.a().ui.askOpen());   // stop the pattern first
    r.tap(btn::F8);
    CHECK(!r.a().seq.isPlaying());
    r.waitJob();
    CHECK(r.a().ui.active() == kModPattern);
    CHECK(std::strcmp(r.a().project.name, "Inner project") == 0);
    CHECK(std::strcmp(r.a().project.folder, "Projects/A folder") == 0);
    r.budget();
}

TEST(file_new_folder_by_keys) {
    Rig r;
    r.openBrowser();
    r.tap(btn::F4);   // New Folder: keyboard "New folder"
    CHECK(r.a().ui.kb.isOpen());
    CHECK(std::strcmp(r.a().ui.kb.text(), "New folder") == 0);
    r.budget();       // the keyboard frame over a full browser fits RAM_DL
    for (int i = 0; i < 12; ++i) r.tap(btn::Delete);
    r.h.type("beats");
    r.frame();
    r.tap(btn::F8);   // Create: made and entered
    CHECK(!r.a().ui.kb.isOpen());
    CHECK(r.h.fs.dirs.count("Projects/beats"));
    CHECK(r.fs().state == 1 && r.fs().count == 1);   // ".."
    r.budget();
}

TEST(file_new_folder_while_playing) {
    Rig r;
    r.play();
    r.openBrowser();
    r.tap(btn::F4);
    CHECK(r.a().ui.kb.isOpen());
    r.budget();
    r.tap(btn::F8);
    CHECK(r.h.fs.dirs.count("Projects/New folder"));
    CHECK(r.a().seq.isPlaying());   // New Folder does not stop playback
    r.budget();
}

TEST(file_open_while_playing_asks_to_stop) {
    Rig r;
    r.play();
    r.openBrowser();
    r.select("B project");
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());   // "This action will stop the pattern / continue?"
    CHECK(r.a().seq.isPlaying());
    r.tap(btn::F7);              // Cancel: still playing, back in the browser
    CHECK(!r.a().ui.askOpen());
    CHECK(r.a().seq.isPlaying());
    CHECK(r.a().ui.active() == kModFile && r.fs().state == 1);
    CHECK(std::strcmp(r.fs().entries[r.fs().cursor], "B project") == 0);
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());
    r.tap(btn::F8);              // Yes: stop, then open
    CHECK(!r.a().seq.isPlaying());
    r.waitJob();
    CHECK(r.a().ui.active() == kModPattern);
    CHECK(std::strcmp(r.a().project.name, "B project") == 0);
    r.budget();
}

TEST(file_open_recent_while_playing_asks_to_stop) {
    Rig r;
    CHECK(r.a().openProject(kProjectsDir, "B project"));
    r.a().newProject();   // B project goes to Recent on leave
    r.play();
    r.openBrowser();
    r.tap(btn::Right);   // Recent list
    CHECK(r.fs().focus == 1);
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());
    r.tap(btn::F8);
    CHECK(!r.a().seq.isPlaying());
    r.waitJob();
    CHECK(r.a().ui.active() == kModPattern);
    CHECK(std::strcmp(r.a().project.name, "B project") == 0);
}

TEST(file_import_while_playing_asks_to_stop) {
    Rig r;
    r.h.fs.dirs.insert("Mod");
    r.h.fs.files["Mod/song.mod"] = std::vector<uint8_t>(64, 0);
    r.play();
    r.tap(btn::File);
    r.tap(btn::F3);   // Import Mod browser
    CHECK(r.fs().state == 2);
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());
    r.tap(btn::F7);
    CHECK(r.a().seq.isPlaying());
    CHECK(r.fs().state == 2);
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());
    r.tap(btn::F8);   // Yes: stopped, the import runs (this file is invalid)
    CHECK(!r.a().seq.isPlaying());
    r.budget();
}

TEST(file_save_as_by_keys) {
    Rig r;
    r.play();
    r.tap(btn::File);
    r.tap(btn::F5);   // Save As browser
    CHECK(r.fs().state == 3);
    r.select("A folder");
    r.tap(btn::F1);   // Enter
    r.tap(btn::F8);   // Save: keyboard with the project name
    CHECK(r.a().ui.kb.isOpen());
    r.budget();
    for (int i = 0; i < 40; ++i) r.h.tap(btn::Delete);
    r.h.type("saved");
    r.tap(btn::F8);
    for (int i = 0; i < 50 && !r.h.fs.files.count("Projects/A folder/saved/project.mt"); ++i) r.h.run(20);
    CHECK(r.h.fs.files.count("Projects/A folder/saved/project.mt"));
    CHECK(std::strcmp(r.a().project.folder, "Projects/A folder") == 0);
    CHECK(r.fs().state == 0);
    r.budget();
}

// Stop prompt, then the unsaved-changes prompt (E1.2), Don't Save opens.
TEST(file_open_while_playing_then_save_prompt) {
    Rig r;
    r.tap(btn::Pattern);
    if (!r.a().editMode) r.tap(btn::Rec);
    r.h.pad(36);
    CHECK(r.a().unsavedChanges());
    r.play();
    r.openBrowser();
    r.select("B project");
    r.tap(btn::F8);
    CHECK(r.a().ui.askOpen());
    r.tap(btn::F8);              // Yes: stopped
    CHECK(!r.a().seq.isPlaying());
    CHECK(r.a().ui.askOpen());   // "Save the changes to ...?"
    r.tap(btn::F7);              // Don't Save
    r.waitJob();
    CHECK(r.a().ui.active() == kModPattern);
    CHECK(std::strcmp(r.a().project.name, "B project") == 0);
    r.budget();
}
