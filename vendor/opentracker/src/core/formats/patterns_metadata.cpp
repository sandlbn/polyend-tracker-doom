// patternsMetadata and recents codecs, spec 02 §5 and §7.
#include "formats/patterns_metadata.h"

#include <cstring>

#include "formats/bytes.h"

namespace ot {

void encodePatternNames(const PatternNames& names, uint8_t out[pamd::kFileSize]) {
    std::memset(out, 0, pamd::kFileSize);
    std::memcpy(out, "PAMD", 4);
    wrU32(out + 4, pamd::kVersion);  // u16 version written as u32 (0x06..0x07 = 0)
    wrU32(out + 8, pamd::kFileSize);
    wrU32(out + 12, 0);              // control flags
    for (uint32_t i = 0; i < pamd::kRecordCount; ++i) {
        uint8_t* rec = out + pamd::kHeaderSize + i * pamd::kRecordSize;
        // char name[30] + NUL forced at +30; +31..+49 zero.
        wrStr(rec, PatternNames::kMaxLen + 1, names.names[i]);
        rec[PatternNames::kMaxLen] = 0;
    }
}

FmtStatus decodePatternNames(const uint8_t* file, size_t len, PatternNames& names) {
    names.clear();
    if (len < 4 || std::memcmp(file, "PAMD", 4) != 0) return FmtStatus::BadMagic;
    for (uint32_t i = 0; i < pamd::kRecordCount; ++i) {
        uint32_t off = pamd::kHeaderSize + i * pamd::kRecordSize;
        uint8_t rec[PatternNames::kMaxLen + 1] = {};
        for (uint32_t k = 0; k < sizeof(rec); ++k) rec[k] = off + k < len ? file[off + k] : 0;
        if (!std::memchr(rec, 0, sizeof(rec))) continue;  // no NUL in first 31 bytes: absent
        rdStr(rec, sizeof(rec), names.names[i], sizeof(names.names[i]));
    }
    return FmtStatus::Ok;
}

void encodeRecents(const Recents& r, uint8_t out[recents::kFileSize]) {
    for (int i = 0; i < Recents::kSlots; ++i)
        wrStr(out + i * Recents::kSlotSize, Recents::kSlotSize, r.paths[i]);
}

void decodeRecents(const uint8_t* file, size_t len, Recents& r) {
    r.clear();
    for (int i = 0; i < Recents::kSlots; ++i) {
        size_t off = size_t(i) * Recents::kSlotSize;
        if (off >= len) break;
        size_t avail = len - off < Recents::kSlotSize ? len - off : Recents::kSlotSize;
        rdStr(file + off, uint32_t(avail), r.paths[i], Recents::kSlotSize);
    }
}

}  // namespace ot
