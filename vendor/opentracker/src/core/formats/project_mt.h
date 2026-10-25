// project.mt codec (spec 02 §3), fixed 0x914 bytes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "model/project.h"

namespace ot {

constexpr uint8_t kProjectStructVersion = 17;          // spec 02 §3.1
constexpr uint8_t kProjectFwVersion[4] = {1, 9, 2, 255};

struct ProjectLoadInfo {
    uint8_t structVersion = 0;   // byte 0x008 as found in the file
    uint16_t sizeField = 0;
    bool sizeMismatch = false;   // size != 0x914 -> "Unsaved Project" path
    bool upgraded = false;       // structure <= 16 (upgrade hook invoked if set)
    uint32_t storedCrc = 0;      // informational only; never checked (spec 02 §2)
    uint32_t computedCrc = 0;
    uint8_t synthSlotsReset = 0; // bit s: slot record s failed the check and took the default
};

// Serialise exactly as fw 1.9.2 does (spec 02 §3.1 "Save").
void encodeProject(const Project& p, uint8_t out[mt::kFileSize]);

// Load per spec 02 §3.1 "Load algorithm" as corrected by spec 10 §0.2:
// `file`/`len` is whatever was read from disk; bytes past `len` read as 0.
// Structure 1..16 runs the spec 10 §1.2 chain to 17. Returns WrongType if
// the type field is not 0 and Unsupported for structure version 0; the
// project is untouched in those cases.
// trackIndexLimit: runtime count N for the 0x152 check (0 = skip check).
FmtStatus decodeProject(const uint8_t* file, size_t len, Project& p, ProjectLoadInfo* info = nullptr,
                        uint8_t trackIndexLimit = 0);

// Value sanitiser applied after load (spec 02 §3.3 "Validation on load"),
// followed by sanitizeSynthSlots (spec 21 §3.2: the Mini validates the slot
// records on every load). Returns sanitizeSynthSlots' mask of reset slots.
uint8_t sanitizeProject(Project& p, uint8_t trackIndexLimit = 0);

// Tracker Mini synth-slot check (spec 21 §3.3): a slot record with any field
// out of range is replaced as a whole by the slot default. bipolarMask[s]
// (optional) gives the bipolar macros of slot s's loaded patch (bit m =
// macro m+1), which default to 63 instead of 0. Returns a bit mask of the
// slots that were reset (bit s = slot s).
uint8_t sanitizeSynthSlots(Project& p, const uint8_t* bipolarMask = nullptr);

// Override for the built-in spec 10 §1.2 upgrade chain (tests). Called
// with the 0x800-byte block at file offset 0x010. nullptr = built-in chain.
void setProjectUpgradeHook(BlockUpgradeFn fn);

}  // namespace ot
