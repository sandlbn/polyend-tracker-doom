// Shared definitions for the file codecs (spec 02).
#pragma once

#include <cstdint>

namespace ot {

enum class FmtStatus : uint8_t {
    Ok = 0,
    WrongType,      // header type field rejected (.mt type != 0, .pti type != 1)
    BadMagic,       // magic mismatch where the spec checks it (PAMD)
    TooShort,       // not enough bytes to interpret at all
    Unsupported,    // e.g. unsupported WAV encoding
    IoError,        // storage failure
    NotFound,       // file missing
    NoSpace,        // destination buffer / sample bank too small
};

// Common 16-byte header used by .mt/.mtp/.pti (spec 02 §3.1, §4.1, §6.1).
constexpr uint32_t kCommonHeaderSize = 0x10;

struct FileHeader {
    char magic[2];
    uint16_t type;
    uint8_t fwVersion[4];
    uint8_t structVersion[4];
    uint16_t size;
};

void writeFileHeader(uint8_t* out, const FileHeader& h);   // 0x0E..0x0F written 0
FileHeader readFileHeader(const uint8_t* in);

// Upgrade hook applied to a raw block when an old structure version is
// loaded (spec 02 §3.1 step 5, §6.1). The individual upgrade steps are not
// specified (spec 02 §12 item 1); by default no hook is installed and old
// files are loaded as-is. `fromVersion` is the structure byte from the file.
using BlockUpgradeFn = void (*)(uint8_t* block, uint32_t len, uint8_t fromVersion);

}  // namespace ot
