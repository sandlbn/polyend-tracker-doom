#include "games/rom_list.h"

#include <cstring>

#include "hal_api.h"

namespace ot {
namespace games {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Position of the first case-insensitive ".nes" in the first `limit` chars, or -1.
int findNes(const char* s, int limit) {
    for (int i = 0; s[i] && i < limit; ++i) {
        if (s[i] == '.' && s[i + 1] && lower(s[i + 1]) == 'n' && s[i + 2] && lower(s[i + 2]) == 'e' && s[i + 3] &&
            lower(s[i + 3]) == 's')
            return i;
    }
    return -1;
}
}  // namespace

bool romNameQualifies(const char* name, bool isDir) {
    if (!name || !name[0] || isDir) return false;
    if (name[0] == '.' && name[1] == '_') return false;
    return findNes(name, 0x7FFFFFFF) >= 0;
}

int romDisplayLength(const char* name) {
    const int p = findNes(name, 49);
    return p >= 0 ? p : int(std::strlen(name));
}

int naturalCompare(const char* a, const char* b) {
    while (*a && *b) {
        if (isDigit(*a) && isDigit(*b)) {
            // Compare the digit runs as numbers (leading zeros ignored).
            while (*a == '0' && isDigit(a[1])) ++a;
            while (*b == '0' && isDigit(b[1])) ++b;
            int la = 0, lb = 0;
            while (isDigit(a[la])) ++la;
            while (isDigit(b[lb])) ++lb;
            if (la != lb) return la < lb ? -1 : 1;
            for (int i = 0; i < la; ++i)
                if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
            a += la;
            b += lb;
            continue;
        }
        if (isDigit(*a) != isDigit(*b)) return isDigit(*a) ? -1 : 1;
        const char ca = lower(*a), cb = lower(*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++a;
        ++b;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;   // the shorter name first
}

bool RomList::onEntry(const IStorage::DirEntry& e, void* ctx) {
    RomList& l = *static_cast<RomList*>(ctx);
    if (!romNameQualifies(e.name, e.isDir)) return true;
    const uint32_t len = uint32_t(std::strlen(e.name));
    // spec 14 §1.4: stop at 101 names or when the name storage is full.
    if (l.count_ >= kMaxRoms || l.used_ + len + 1 > kRomNamePool) return false;
    std::memcpy(l.pool_ + l.used_, e.name, len + 1);
    l.off_[l.count_++] = uint16_t(l.used_);
    l.used_ += len + 1;
    return true;
}

int RomList::scan(IStorage& fs) {
    count_ = 0;
    used_ = 0;
    if (!fs.exists(kGamesDir) && !fs.mkdir(kGamesDir)) return -1;
    if (fs.listDir(kGamesDir, &RomList::onEntry, this) < 0) {
        count_ = 0;
        return -1;
    }
    // Insertion sort of the offsets (<= 101 names).
    for (int i = 1; i < count_; ++i) {
        const uint16_t k = off_[i];
        int j = i - 1;
        while (j >= 0 && naturalCompare(pool_ + off_[j], pool_ + k) > 0) {
            off_[j + 1] = off_[j];
            --j;
        }
        off_[j + 1] = k;
    }
    return count_;
}

void RomList::displayName(int i, char* out, int cap) const {
    if (cap <= 0) return;
    out[0] = 0;
    if (i < 0 || i >= count_) return;
    const char* n = pool_ + off_[i];
    int len = romDisplayLength(n);
    if (len > cap - 1) len = cap - 1;
    std::memcpy(out, n, size_t(len));
    out[len] = 0;
}

}  // namespace games
}  // namespace ot
