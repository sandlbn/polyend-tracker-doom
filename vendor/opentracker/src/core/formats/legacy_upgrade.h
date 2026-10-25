// Legacy file upgrades (spec 10): project.mt v1..16 -> v17, pattern .mtp
// v1..4 -> v5, instrument .mti/.pti v1..8 -> v9, and building a full .pti
// from a header-only .mti plus its samples/instrNN.wav.
//
// Every function works on a raw, zero-filled fixed-size file image (spec 10
// §0.1). All offsets are FILE offsets. The step functions are exposed so
// they can be tested one by one; loaders normally only call the drivers,
// which decodeProject / decodePattern / decodePtiHeader already do.
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "formats/wav.h"
#include "model/instrument.h"
#include "model/pattern.h"

namespace ot {
namespace legacy {

// --- gain curves (spec 10 §0.4), bit-exact single precision -------------
uint8_t gainG1(uint8_t v);   // -19.8..0 dB -> -36..+6 dB
uint8_t gainG2(uint8_t v);   // -19.8..0 dB -> -24..+24 dB
uint8_t gainG3(uint8_t v);   // -36..+6 dB  -> -18..+3 dB
uint8_t gainG4(uint8_t v);   // -24..+24 dB -> -12..+12 dB
uint16_t logLG(uint16_t v);  // linear amplitude -> log-scaled (spec 10 §0.4 "LG")

// --- project.mt (spec 10 §1) ---------------------------------------------
constexpr uint32_t kProjectImageSize = 0x914;   // = mt::kFileSize
constexpr uint8_t kProjectTargetVersion = 17;

// One step v -> v+1 on the whole 0x914-byte image (from = 1..16).
// `counter` is the byte counter C of spec 10 §1.2 (only read by step 15).
void projectStep(uint8_t* img, uint8_t from, uint32_t& counter);

// Driver: runs steps fromVersion..16 in place (spec 10 §0.3). Returns false
// (image untouched) for version 0, which is rejected (spec 10 §0.3
// recommendation). Versions >= 17 run nothing and return true. The header
// version/size bytes are NOT changed (spec 10 §1.2, §1.3).
bool upgradeProjectImage(uint8_t* img, uint8_t fromVersion);

// --- pattern .mtp (spec 10 §2) -------------------------------------------
struct PatternUpgradeParams {
    // Play mode per sample instrument 0..47, AFTER the instrument upgrade
    // (spec 10 §2.2 ordering). nullptr skips step (b) (SPEC GAP: no data).
    const uint8_t* instrumentPlayModes = nullptr;
    uint8_t beatSliceReference = 24;   // live project byte 0x117
    // Overrides for tests; nullptr = spec curves G2 / G4.
    uint8_t (*curveV2)(uint8_t) = nullptr;
    uint8_t (*curveV4)(uint8_t) = nullptr;
};

// Steps a..d of spec 10 §2.2 on the decoded pattern, given the file's
// structure byte (only runs when < 5). Does not do the separate size-field
// reset (decodePattern does). Returns true if anything ran.
// `tracks` = the first `trackCount` tracks of the pattern: kStockTrackCount
// (12) for the stock 1.9.x layout (step d clears tracks 8..11), or
// kMiniTrackCount (16) for the Tracker Mini layout (spec 21 §4.2 step 2:
// step d clears tracks 8..15). Steps a..c only touch tracks 0..7.
bool upgradePattern(Track* tracks, uint8_t fromVersion, const PatternUpgradeParams& prm,
                    int trackCount = kStockTrackCount);
void patternStepVolume(Track* tracks, uint8_t (*curve)(uint8_t));                  // a / c
void patternStepBeatSlice(Track* tracks, const uint8_t* modes, uint8_t ref117);    // b
// d: tracks 8..trackCount-1 become empty with track 0's length.
void patternStepExpandTracks(Track* tracks, int trackCount = kStockTrackCount);
inline bool upgradePattern(PatternStock& p, uint8_t v, const PatternUpgradeParams& prm) {
    return upgradePattern(p.tracks, v, prm);
}
inline bool upgradePattern(Pattern& p, uint8_t v, const PatternUpgradeParams& prm) {
    return upgradePattern(p.tracks, v, prm);
}

// --- instrument .mti / .pti header (spec 10 §3) ---------------------------
constexpr uint32_t kPtiHeaderImageSize = 0x188;
constexpr uint8_t kPtiTargetVersion = 9;

// One step v -> v+1 on the 0x188-byte header image (from = 1..8).
void ptiStep(uint8_t* hdr, uint8_t from);
// Driver: steps fromVersion..8 (version 0 is treated as 1, spec 10 §0.3).
// Header version/size bytes are not changed.
void upgradePtiImage(uint8_t* hdr, uint8_t fromVersion);

// Read a .mti/.pti header the way the firmware does: zero-fill 0x188 bytes,
// copy min(len, 0x188), require type 1, run the chain. `dataOffset` =
// size field + 0x14. The image is left un-normalised (old version bytes).
FmtStatus readLegacyPtiHeader(const uint8_t* file, size_t len, uint8_t hdr[kPtiHeaderImageSize],
                              uint8_t* structVersion = nullptr, uint32_t* dataOffset = nullptr);

// spec 10 §3.3 step 2/3: is the slot accepted, and does it need the WAV path?
inline bool ptiSlotAccepted(size_t fileLen, uint32_t dataOffset) { return fileLen >= dataOffset; }
inline bool ptiNeedsWav(size_t fileLen, uint32_t dataOffset) { return fileLen == dataOffset; }

// spec 10 §3.3 step 3: frame count used for the memory check of a legacy
// slot = min(dataChunkBytes, wavFileLength) / 2.
inline uint32_t legacyWavCheckFrames(const WavInfo& w, uint32_t wavFileLen) {
    return (w.dataSize < wavFileLen ? w.dataSize : wavFileLen) / 2;
}

// spec 10 §3.3 step 5: apply the imported frame count to the instrument
// (length; grain length clamped to the length).
void applyImportedLength(Instrument& ins, uint32_t frames, bool stereo);

// Build a complete v9 .pti in memory from a legacy header + WAV file
// (spec 10 §3.3 steps 2-7): header upgraded and decoded, WAV imported with
// the normal importer into `scratch` (capacity in int16 samples), then
// header (rewritten as v9: TI/1/1.9.2.1/9x4/0x174/CRC) + payload at 0x188
// written to `out`. Returns the file size, 0 on failure (`*status` says why).
// `insOut` (optional) receives the decoded instrument.
size_t buildPtiFromMti(const uint8_t* mti, size_t mtiLen, const uint8_t* wavFile, size_t wavLen,
                       int16_t* scratch, size_t scratchCapSamples, uint8_t* out, size_t outCap,
                       Instrument* insOut = nullptr, FmtStatus* status = nullptr, bool stereo = false);

}  // namespace legacy
}  // namespace ot
