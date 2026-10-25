// Project / folder browser (spec 05 appendix E1.2).
#include "app/project_browser.h"

#include <cstdio>
#include <cstring>

#include "app/app.h"

namespace ot {

namespace {
struct Collect {
    char (*names)[ProjectBrowser::kNameCap];
    int cap;
    int count;
};
bool collectDirs(const IStorage::DirEntry& e, void* ctx) {
    auto* c = static_cast<Collect*>(ctx);
    if (!e.isDir || e.name[0] == '.') return true;
    // SPEC GAP: names that do not fit the 40-char list rows are not listed
    // (projects and keyboard-made folders are at most 32 chars).
    if (std::strlen(e.name) >= size_t(ProjectBrowser::kNameCap)) return true;
    if (c->count >= c->cap) return false;
    std::snprintf(c->names[c->count++], ProjectBrowser::kNameCap, "%s", e.name);
    return true;
}

int cmpNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}
}  // namespace

bool isActiveProjectPath(App& a, const char* path, bool* contains) {
    if (contains) *contains = false;
    // A never-saved project has no folder on the card (spec 05 appendix E2).
    if (a.neverSaved() || !a.project.name[0] || !a.project.folder[0]) return false;
    char active[mt::kFolderMax + mt::kNameMax + 2];
    std::snprintf(active, sizeof(active), "%s/%s", a.project.folder, a.project.name);
    if (std::strcmp(active, path) == 0) return true;
    const size_t n = std::strlen(path);
    if (std::strncmp(active, path, n) == 0 && active[n] == '/') {
        if (contains) *contains = true;
        return true;
    }
    return false;
}

void ProjectBrowser::bind(char (*names)[kNameCap], int cap) {
    names_ = names;
    cap_ = cap > kMaxEntries ? kMaxEntries : cap;
    count_ = 0;
}

bool ProjectBrowser::atRoot() const { return std::strcmp(path_, root_) == 0; }

void ProjectBrowser::reset(App& a, const char* root) {
    std::snprintf(root_, sizeof(root_), "%s", root);
    std::snprintf(path_, sizeof(path_), "%s", root);
    a.fs().mkdir(root_);   // spec 05 appendix E1.2: created when missing
    list(a);
}

int ProjectBrowser::list(App& a) {
    count_ = 0;
    if (!names_ || cap_ <= 0) return 0;
    IStorage& f = a.fs();
    // SPEC GAP: the way back up is a ".." row on top of every sub-folder
    // listing (Enter on it); the stock navigation upwards is not specified.
    int first = 0;
    if (!atRoot()) {
        std::snprintf(names_[0], kNameCap, "..");
        kind_[0] = kUp;
        first = 1;
    }
    Collect c{names_ + first, cap_ - first, 0};
    f.listDir(path_, collectDirs, &c);
    count_ = first + c.count;
    // Kind: a folder holding project.mt is a project (spec 05 appendix E1.2).
    char p[kPathMax + kNameCap + 16];
    for (int i = first; i < count_; ++i) {
        std::snprintf(p, sizeof(p), "%s/%s/project.mt", path_, names_[i]);
        kind_[i] = f.exists(p) ? kProject : kFolder;
    }
    // SPEC GAP: listing order is not specified; case-insensitive alphabetical.
    for (int i = first + 1; i < count_; ++i)
        for (int j = i; j > first && cmpNoCase(names_[j - 1], names_[j]) > 0; --j) {
            char t[kNameCap];
            std::memcpy(t, names_[j], kNameCap);
            std::memcpy(names_[j], names_[j - 1], kNameCap);
            std::memcpy(names_[j - 1], t, kNameCap);
            const uint8_t k = kind_[j];
            kind_[j] = kind_[j - 1];
            kind_[j - 1] = k;
        }
    return count_;
}

bool ProjectBrowser::up(App& a) {
    if (atRoot()) return false;
    char* s = std::strrchr(path_, '/');
    if (!s) return false;
    *s = 0;
    list(a);
    return true;
}

bool ProjectBrowser::enter(App& a, int i) {
    if (!valid(i)) return false;
    if (kind_[i] == kUp) return up(a);
    if (kind_[i] != kFolder) return false;
    const size_t need = std::strlen(path_) + 1 + std::strlen(names_[i]);
    if (need > size_t(kPathMax)) return false;
    std::strcat(path_, "/");
    std::strcat(path_, names_[i]);
    list(a);
    return true;
}

ProjectBrowser::Result ProjectBrowser::newFolder(App& a, const char* name) {
    if (!name || !name[0] || std::strchr(name, '/') || !std::strcmp(name, ".") || !std::strcmp(name, ".."))
        return Result::Invalid;
    const size_t n = std::strlen(name);
    if (n > size_t(kFolderNameMax)) return Result::Invalid;
    // spec 05 appendix E1.2: len(path) + len(name) + 2 >= 253 -> refused.
    if (std::strlen(path_) + n + 2 >= size_t(kPathMax)) return Result::TooLong;
    char p[kPathMax + kFolderNameMax + 2];
    std::snprintf(p, sizeof(p), "%s/%s", path_, name);
    if (a.fs().exists(p)) return Result::Exists;
    if (!a.fs().mkdir(p)) return Result::IoError;
    std::memcpy(path_, p, std::strlen(p) + 1);   // the browser enters it (fits: checked above)
    list(a);
    return Result::Ok;
}

void ProjectBrowser::entryPath(int i, char* out, int cap) const {
    std::snprintf(out, size_t(cap), "%s/%s", path_, name(i));
}

int ProjectBrowser::find(const char* n) const {
    for (int i = 0; i < count_; ++i)
        if (kind_[i] != kUp && std::strcmp(names_[i], n) == 0) return i;
    return -1;
}

ProjectBrowser::Result ProjectBrowser::canDelete(App& a, int i) const {
    if (!valid(i) || kind_[i] == kUp) return Result::Invalid;
    char p[kPathMax + kNameCap + 2];
    entryPath(i, p, sizeof(p));
    bool contains = false;
    if (isActiveProjectPath(a, p, &contains)) return contains ? Result::ContainsActive : Result::Active;
    return Result::Ok;
}

ProjectBrowser::Result ProjectBrowser::remove(App& a, int i) {
    const Result r = canDelete(a, i);
    if (r != Result::Ok) return r;
    char p[kPathMax + kNameCap + 2];
    entryPath(i, p, sizeof(p));
    // spec 05 appendix E1.2: recursive folder delete, then list refresh.
    const bool ok = a.removeTree(p);
    list(a);
    return ok ? Result::Ok : Result::IoError;
}

}  // namespace ot
