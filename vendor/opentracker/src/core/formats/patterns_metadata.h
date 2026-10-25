// patternsMetadata ("PAMD", spec 02 §5) and Projects/recents (spec 02 §7) codecs.
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "model/project.h"

namespace ot {

namespace pamd {
constexpr uint32_t kFileSize = 0x3210;   // 12816
constexpr uint32_t kHeaderSize = 0x10;
constexpr uint32_t kRecordSize = 50;
constexpr uint32_t kRecordCount = 256;
constexpr uint32_t kVersion = 1;
}  // namespace pamd

void encodePatternNames(const PatternNames& names, uint8_t out[pamd::kFileSize]);
// Magic mismatch clears all names and returns BadMagic (spec 02 §5). A name
// is kept only if a NUL occurs within its first 31 bytes.
FmtStatus decodePatternNames(const uint8_t* file, size_t len, PatternNames& names);

namespace recents {
constexpr uint32_t kFileSize = Recents::kSlots * Recents::kSlotSize;  // 5000
}

void encodeRecents(const Recents& r, uint8_t out[recents::kFileSize]);
void decodeRecents(const uint8_t* file, size_t len, Recents& r);

}  // namespace ot
