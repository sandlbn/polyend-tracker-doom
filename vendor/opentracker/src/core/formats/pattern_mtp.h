// pattern_NN.mtp codec.
//  - 12-track API (encodePattern / decodePattern): stock 1.9.x, spec 02 §4,
//    fixed 0x242C bytes.
//  - 16-track API (encodePattern16 / decodePattern16): Tracker Mini 2.2.1,
//    spec 21 §4, 0x3030 bytes (or the stock layout on request).
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "model/pattern.h"

namespace ot {

namespace mtp {
// Stock 1.9.x layout (12 tracks). kFileSize / kCrcOff keep naming the stock
// layout for the existing 12-track API below.
constexpr uint32_t kFileSize = 0x242C;      // 9260
constexpr uint32_t kLegacyFileSize = 6184;  // 8-track variant (0x1C + 769*8 + 4)
constexpr uint32_t kReservedOff = 0x10;     // 12 reserved bytes, written 0
constexpr uint32_t kTracksOff = 0x1C;
constexpr uint32_t kTrackSize = 0x301;      // 769
constexpr uint32_t kStepSize = 6;
constexpr uint32_t kCrcOff = 0x2428;
constexpr uint8_t kStructVersion = 5;
constexpr uint8_t kFwVersion[4] = {1, 9, 2, 1};

// Size of a pattern file holding `tracks` tracks: header + reserved (0x1C),
// the tracks, CRC (spec 21 §4.1; tracker-lib PatternConstants).
constexpr uint32_t fileSizeFor(int tracks) { return kTracksOff + kTrackSize * uint32_t(tracks) + 4; }
constexpr uint32_t crcOffFor(int tracks) { return kTracksOff + kTrackSize * uint32_t(tracks); }

constexpr uint32_t kStockFileSize = kFileSize;             // 0x242C, 12 tracks
constexpr uint32_t kStockCrcOff = kCrcOff;                 // 0x2428
// Tracker Mini 2.2.1 layout (spec 21 §4.1): 16 tracks.
constexpr uint32_t kMiniFileSize = 0x3030;                 // 12 336
constexpr uint32_t kMiniCrcOff = 0x302C;
constexpr uint32_t kMaxFileSize = kMiniFileSize;
static_assert(fileSizeFor(12) == kStockFileSize && crcOffFor(12) == kStockCrcOff, "stock layout");
static_assert(fileSizeFor(16) == kMiniFileSize && crcOffFor(16) == kMiniCrcOff, "Mini layout");
static_assert(fileSizeFor(8) == kLegacyFileSize, "8-track layout");
}  // namespace mtp

// Which on-disk layout encodePattern16 writes.
enum class MtpLayout : uint8_t {
    Mini16,    // 0x3030, 16 tracks (2.x native, spec 21 §4.1)
    Stock12,   // 0x242C, tracks 0..11 only (stock 1.9.x layout; tracks 12..15 dropped)
};

struct PatternLoadInfo {
    uint8_t structVersion = 0;
    uint16_t sizeField = 0;
    bool upgraded = false;         // structure < 5
    bool upperTracksReset = false; // tracks 8..11 were reset (v<5 or size != 0x242C)
                                   // (16-track decoder: any of tracks 8..15 reset)
    uint8_t firstResetTrack = 0;   // 16-track decoder only: first track reset to
                                   // empty (8 or 12), 16 = none
    uint32_t storedCrc = 0;        // informational only (spec 02 §2)
    uint32_t computedCrc = 0;
};

// Inputs for the pre-v5 upgrade chain (spec 10 §2.2).
struct PatternUpgradeContext {
    // Play mode per sample instrument (0..47); nullptr skips the v<3 step.
    const uint8_t* instrumentPlayModes = nullptr;
    // Project byte 0x117 (spec 02 §3.3); v<3 transposes by (12 - value).
    uint8_t beatSliceReference = 24;
    // FX 18 (Volume) value curves for v<2 and v<4. nullptr = the spec 10
    // §0.4 curves G2 / G4 (overrides are for tests).
    uint8_t (*volumeCurveV2)(uint8_t) = nullptr;
    uint8_t (*volumeCurveV4)(uint8_t) = nullptr;
};

// Save per spec 02 §4.4 "Save": header KS / 2 / 1.9.2.1 / 5.5.5.5 / 0x242C, CRC.
// The file holds the kStockTrackCount (12) stock tracks; a 2.x Pattern's
// tracks 12..15 are not written (2.x writes 16 tracks with encodePattern16).
void encodePattern(const PatternStock& p, uint8_t out[mtp::kFileSize]);
void encodePattern(const Pattern& p, uint8_t out[mtp::kFileSize]);

// Load per spec 02 §4.4 "Load". Never fails: ids/type/CRC are not checked,
// missing bytes read as zero. The Pattern overload resets tracks 12..15 to
// empty tracks of track 0's length.
void decodePattern(const uint8_t* file, size_t len, PatternStock& p, const PatternUpgradeContext* ctx = nullptr,
                   PatternLoadInfo* info = nullptr);
void decodePattern(const uint8_t* file, size_t len, Pattern& p, const PatternUpgradeContext* ctx = nullptr,
                   PatternLoadInfo* info = nullptr);

// --- 16-track API (spec 21 §4) ---------------------------------------------
//
// Encode: header KS / 2 / 1.9.2.1 / 5.5.5.5 / size, reserved 0, tracks, CRC-32
// (ISO-HDLC, as stock: spec 21 U1). Mini16 writes all 16 tracks (0x3030 B,
// CRC at 0x302C); Stock12 writes tracks 0..11 exactly like encodePattern
// (0x242C B). `out` must hold mtp::kMiniFileSize (Mini16) or
// mtp::kStockFileSize (Stock12) bytes. Returns the number of bytes written.
size_t encodePattern16(const Track tracks[kTrackCount], uint8_t* out, MtpLayout layout = MtpLayout::Mini16);
size_t encodePattern16(const Pattern& p, uint8_t* out, MtpLayout layout = MtpLayout::Mini16);
// From the resident stock tracks plus the ext-pool tracks 12..15
// (ExtTrackPool::find(num, e)); a null ext[e] is the default ext track
// (empty, track 0's length). `ext` itself may be null (all default).
size_t encodePattern16(const PatternStock& s, const Track* const ext[kExtTrackCount], uint8_t* out,
                       MtpLayout layout = MtpLayout::Mini16);

// Decode per spec 21 §4.2. Never fails; ids/type/CRC are not checked; up to
// 0x3030 bytes are read and missing bytes read as zero.
//  - structure < 5: the v<2/v<3/v<4 conversions of spec 10 §2.2 on tracks
//    0..7, then tracks 8..15 are cleared (length = track 0's);
//  - size field 0x3030: nothing else;
//  - size field 0x242C (12-track 1.9.x file): tracks 12..15 reset;
//  - any other size (8-track 6 184 B file, ...): tracks 8..15 reset.
void decodePattern16(const uint8_t* file, size_t len, Track tracks[kTrackCount],
                     const PatternUpgradeContext* ctx = nullptr, PatternLoadInfo* info = nullptr);
void decodePattern16(const uint8_t* file, size_t len, Pattern& p, const PatternUpgradeContext* ctx = nullptr,
                     PatternLoadInfo* info = nullptr);
// Split target: stock tracks 0..11 into `s`, tracks 12..15 into ext[0..3].
void decodePattern16(const uint8_t* file, size_t len, PatternStock& s, Track ext[kExtTrackCount],
                     const PatternUpgradeContext* ctx = nullptr, PatternLoadInfo* info = nullptr);

}  // namespace ot
