// Project / folder browser of the File screen Open and Save pages
// (spec 05 appendix E1.2). Pure model: the File screen draws it.
#pragma once

#include <cstdint>

namespace ot {

class App;

class ProjectBrowser {
public:
    // Stock lists 0x50 entries (spec 05 appendix E1.2); 2.x lists 200 [OT]
    // (user report: large libraries were cut off). Storage is SDRAM.
    static constexpr int kMaxEntries = 200;
    static constexpr int kNameCap = 40;       // bound to FileScreen::entries[][40]
    static constexpr int kPathMax = 253;      // spec 05 appendix E1.2 New Folder limit
    static constexpr int kFolderNameMax = 32; // keyboard max length

    enum Kind : uint8_t { kUp = 0, kFolder = 1, kProject = 2 };
    enum class Result : uint8_t { Ok, TooLong, Exists, Invalid, IoError, Active, ContainsActive };

    // Entry names are stored in caller-owned storage (cap entries of 40 chars).
    void bind(char (*names)[kNameCap], int cap);
    // Go to the root folder (created when missing) and list it.
    void reset(App& a, const char* root);
    // Re-read the current folder. Returns the entry count.
    int list(App& a);

    int count() const { return count_; }
    const char* name(int i) const { return valid(i) ? names_[i] : ""; }
    Kind kind(int i) const { return valid(i) ? Kind(kind_[i]) : kUp; }
    bool isProject(int i) const { return valid(i) && kind_[i] == kProject; }
    bool isFolder(int i) const { return valid(i) && kind_[i] == kFolder; }
    bool isUp(int i) const { return valid(i) && kind_[i] == kUp; }
    const char* path() const { return path_; }
    const char* root() const { return root_; }
    bool atRoot() const;

    // Btn0 "Enter": a folder (or "..") is entered; a project is a no-op
    // (spec 05 appendix E1.2 0x4577c). Returns true if the folder changed.
    bool enter(App& a, int i);
    bool up(App& a);
    // New Folder (spec 05 appendix E1.2 0x4646c): limits, "already exists",
    // then create and enter it.
    Result newFolder(App& a, const char* name);
    // Delete refusal rules (spec 05 appendix E1.2 0x454ec): the active project
    // or a folder that contains it cannot be deleted.
    Result canDelete(App& a, int i) const;
    Result remove(App& a, int i);
    // "<path>/<name>" of entry i.
    void entryPath(int i, char* out, int cap) const;
    // Index of an entry by name (-1 if absent).
    int find(const char* name) const;

private:
    bool valid(int i) const { return i >= 0 && i < count_; }
    char root_[24] = "Projects";
    char path_[kPathMax + 1] = "Projects";
    char (*names_)[kNameCap] = nullptr;
    int cap_ = 0;
    int count_ = 0;
    uint8_t kind_[kMaxEntries] = {};
};

// Is "<path>" the active project's folder, or does it contain it?
bool isActiveProjectPath(App& a, const char* path, bool* contains = nullptr);

}  // namespace ot
