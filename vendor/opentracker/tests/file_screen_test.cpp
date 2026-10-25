// File screen: Save into the project's own folder, Save As from the
// unsaved-changes prompt followed by the pending action (spec 05 appendix E1, E3).
#include "test.h"
#include "ui_fakes.h"

#include "app/project_io.h"
#include "formats/file_io.h"
#include "ui/screens.h"

using namespace ot;

static FileScreen& fileScr(Harness& h) { return *static_cast<FileScreen*>(h.app->ui.screen(kModFile)); }

static void clearKb(Harness& h) {
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
}

static void enterNote(Harness& h, uint8_t pad) {
    App& a = *h.app;
    h.tap(btn::Pattern);
    if (!a.editMode) h.tap(btn::Rec);
    h.pad(pad);
}

// First non-empty note on track 1 of a saved pattern (kNoteEmpty if none).
static int8_t savedNote(Harness& h, const char* path) {
    Pattern p;
    if (loadPatternFile(h.fs, path, p) != FmtStatus::Ok) return -128;
    for (const auto& st : p.tracks[0].steps)
        if (st.note != kNoteEmpty) return st.note;
    return kNoteEmpty;
}

static bool savedHas(Harness& h, const char* path, int8_t note) {
    Pattern p;
    if (loadPatternFile(h.fs, path, p) != FmtStatus::Ok) return false;
    for (const auto& st : p.tracks[0].steps)
        if (st.note == note) return true;
    return false;
}

TEST(new_project_is_never_saved) {
    Harness h;
    App& a = *h.app;
    CHECK(a.neverSaved());
    CHECK(!a.project.folder[0]);
    CHECK(!canDirectSave(a));
    // Save (F4) on a never-saved project behaves like Save As (E1.3).
    h.tap(btn::File);
    h.tap(btn::F4);
    CHECK(fileScr(h).state == 3);   // Save browser
    CHECK(!a.ui.kb.isOpen());
    h.tap(btn::F8);                 // Save -> keyboard
    CHECK(a.ui.kb.isOpen());
}

TEST(save_into_original_folder) {
    Harness h;
    App& a = *h.app;
    enterNote(h, 36);
    CHECK(saveProjectInto(a, "Other/Sub", "song2"));
    CHECK(h.fs.files.count("Other/Sub/song2/project.mt"));
    CHECK(std::strcmp(a.project.folder, "Other/Sub") == 0);
    h.fs.files["Other/Sub/song2/cover"] = {1, 2, 3};
    // Leave it, then reopen it from Recents.
    h.tap(btn::File);
    h.tap(btn::F1);
    CHECK(std::strcmp(a.project.name, "New Project") == 0);
    h.tap(btn::F2);
    h.tap(btn::Right);
    h.tap(btn::F8);
    CHECK(a.ui.active() == kModPattern);
    CHECK(std::strcmp(a.project.name, "song2") == 0);
    CHECK(std::strcmp(a.project.folder, "Other/Sub") == 0);
    CHECK(canDirectSave(a));
    // Edit and Save: lands in the original folder, not Projects/.
    enterNote(h, 40);
    CHECK(a.unsavedChanges());
    h.tap(btn::File);
    h.tap(btn::F4);
    CHECK(!a.ui.kb.isOpen());
    CHECK(!a.unsavedChanges());
    CHECK(savedHas(h, "Other/Sub/song2/patterns/pattern_01.mtp", 40));
    CHECK(!h.fs.files.count("Projects/song2/project.mt"));
    CHECK(h.fs.files.count("Other/Sub/song2/cover"));   // other files kept
    // The folder stored in project.mt is the original one.
    Project p;
    CHECK(loadProjectFile(h.fs, "Other/Sub/song2/project.mt", p) == FmtStatus::Ok);
    CHECK(std::strcmp(p.folder, "Other/Sub") == 0);
    // Save As still goes to the browser root.
    h.tap(btn::F5);
    h.tap(btn::F8);
    clearKb(h);
    h.type("copy");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Projects/copy/project.mt"));
    CHECK(std::strcmp(a.project.folder, kProjectsDir) == 0);
}

TEST(save_as_then_pending_new) {
    Harness h;
    App& a = *h.app;
    enterNote(h, 36);
    CHECK(a.unsavedChanges());
    h.tap(btn::File);
    h.tap(btn::F1);   // New -> "save changes?"
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);   // Save on a never-saved project -> Save browser
    CHECK(!a.ui.askOpen());
    CHECK(fileScr(h).state == 3);
    h.tap(btn::F8);   // Save -> keyboard
    CHECK(a.ui.kb.isOpen());
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    clearKb(h);
    h.type("first");
    h.tap(btn::F8);
    CHECK(!a.ui.kb.isOpen());
    CHECK(savedNote(h, "Projects/first/patterns/pattern_01.mtp") == 36);
    // ...then the pending New ran.
    CHECK(std::strcmp(a.project.name, "New Project") == 0);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    CHECK(a.neverSaved());
    CHECK(fileScr(h).pending == 0);
    CHECK(fileScr(h).state == 0);
}

TEST(save_as_then_pending_open) {
    Harness h;
    App& a = *h.app;
    enterNote(h, 38);
    CHECK(saveProjectInto(a, kProjectsDir, "song1"));
    h.tap(btn::File);
    h.tap(btn::F1);   // saved, nothing unsaved -> new at once
    CHECK(!a.ui.askOpen());
    enterNote(h, 45);
    h.tap(btn::File);
    h.tap(btn::F2);
    CHECK(fileScr(h).count == 1);
    h.tap(btn::F8);   // Open song1 -> "save changes?"
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    h.tap(btn::F8);
    CHECK(a.ui.kb.isOpen());
    clearKb(h);
    h.type("draft");
    h.tap(btn::F8);
    CHECK(savedNote(h, "Projects/draft/patterns/pattern_01.mtp") == 45);
    CHECK(a.ui.active() == kModPattern);
    CHECK(std::strcmp(a.project.name, "song1") == 0);
    CHECK(a.pattern().tracks[0].steps[0].note == 38);
}

TEST(save_as_cancel_drops_pending) {
    Harness h;
    App& a = *h.app;
    enterNote(h, 36);
    h.tap(btn::File);
    h.tap(btn::F1);
    h.tap(btn::F8);
    h.tap(btn::F8);
    CHECK(a.ui.kb.isOpen());
    h.tap(btn::F7);   // keyboard Cancel -> back to the Save browser
    CHECK(!a.ui.kb.isOpen());
    CHECK(fileScr(h).state == 3);
    CHECK(fileScr(h).pending == 1);
    h.tap(btn::F7);   // browser Cancel -> File main, pending dropped
    CHECK(fileScr(h).state == 0);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    CHECK(a.unsavedChanges());
    CHECK(fileScr(h).pending == 0);
}

TEST(save_as_overwrite_cancel_returns_to_keyboard) {
    Harness h;
    App& a = *h.app;
    CHECK(saveProjectInto(a, kProjectsDir, "taken"));
    enterNote(h, 36);
    h.tap(btn::File);
    h.tap(btn::F1);
    h.tap(btn::F8);   // direct save (project exists) then New
    CHECK(std::strcmp(a.project.name, "New Project") == 0);
    CHECK(savedNote(h, "Projects/taken/patterns/pattern_01.mtp") == 36);
    h.tap(btn::F5);
    h.tap(btn::F8);
    clearKb(h);
    h.type("taken");
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());   // overwrite?
    h.tap(btn::F7);          // Cancel -> back to the keyboard
    CHECK(a.ui.kb.isOpen());
    CHECK(std::strcmp(a.ui.kb.text(), "taken") == 0);
    h.tap(btn::F8);
    h.tap(btn::F8);          // Overwrite
    CHECK(!a.ui.kb.isOpen());
    CHECK(savedNote(h, "Projects/taken/patterns/pattern_01.mtp") == kNoteEmpty);
}

TEST(new_while_playing_asks_to_stop) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::File);
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    h.tap(btn::F1);
    CHECK(a.ui.askOpen());
    CHECK(a.seq.isPlaying());
    h.tap(btn::F7);   // Cancel
    CHECK(a.seq.isPlaying());
    h.tap(btn::F1);
    h.tap(btn::F8);   // Yes
    CHECK(!a.seq.isPlaying());
    // Export page too (E1.4).
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    h.tap(btn::F6);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!a.seq.isPlaying());
    CHECK(fileScr(h).state == 4);
}
