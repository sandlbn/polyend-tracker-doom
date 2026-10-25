// Games browser ROM listing (spec 14 §1.4): `Games/*.nes` on the card,
// natural-sorted, at most 101 names / ~2.7 KB of name storage.
#pragma once

#include <cstdint>

#include "hal_api.h"

namespace ot {
namespace games {

constexpr const char* kGamesDir = "Games";
constexpr int kMaxRoms = 101;            // spec 14 §1.4
constexpr uint32_t kRomNamePool = 2746;  // spec 14 §1.4: 3000 - 254 bytes of names

// spec 14 §1.4 filter: not a directory, not "._*" (macOS resource forks), and
// the lower-cased name contains ".nes" anywhere (substring, not suffix).
// SPEC GAP: IStorage has no FAT attributes, so "hidden" cannot be checked.
bool romNameQualifies(const char* name, bool isDir);
// spec 14 §1.4 natural order: digit runs compare numerically, a digit sorts
// before a non-digit, other characters compare case-insensitively, and when
// one name ends the shorter sorts first. <0, 0, >0 like strcmp.
int naturalCompare(const char* a, const char* b);
// Length of the display name: up to the first case-insensitive ".nes" within
// the first 49 characters, else the whole name (spec 14 §1.4).
int romDisplayLength(const char* name);

class RomList {
public:
    // List Games/ (created when missing). Returns the count, or -1 when the
    // folder cannot be created or read.
    int scan(IStorage& fs);
    int count() const { return count_; }
    // File name inside Games/ (OpenTracker loads by the listed name; stock
    // appends ".nes" to the display name, spec 14 §1.6).
    const char* fileName(int i) const { return i >= 0 && i < count_ ? pool_ + off_[i] : ""; }
    void displayName(int i, char* out, int cap) const;

private:
    static bool onEntry(const IStorage::DirEntry& e, void* ctx);
    char pool_[kRomNamePool + 1] = {};
    uint16_t off_[kMaxRoms] = {};
    int count_ = 0;
    uint32_t used_ = 0;
};

}  // namespace games
}  // namespace ot
