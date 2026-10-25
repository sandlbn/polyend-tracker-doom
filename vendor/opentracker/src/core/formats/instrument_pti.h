// Instrument .pti codec (spec 02 §6): 16-byte header, 372-byte body,
// CRC at 0x184, int16 sample payload from 0x188 (mono or planar stereo).
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "model/instrument.h"

namespace ot {

namespace pti {
constexpr uint32_t kBodyOff = 0x10;
constexpr uint32_t kBodySize = 0x174;     // 372
constexpr uint32_t kCrcOff = 0x184;
constexpr uint32_t kDataOff = 0x188;      // = body size + 0x14
constexpr uint32_t kHeaderBlockSize = kDataOff;
constexpr uint8_t kStructVersion = 9;
constexpr uint8_t kFwVersion[4] = {1, 9, 2, 1};
}  // namespace pti

struct PtiLoadInfo {
    uint8_t structVersion = 0;
    uint16_t bodySize = 0;
    uint32_t dataOffset = 0;   // body size + 0x14
    bool upgraded = false;     // structure < 9
};

// How the payload is interpreted (spec 02 §6.3).
struct PtiSampleLayout {
    uint32_t frames = 0;       // per channel
    bool stereoInFile = false; // payload holds L block + R block
    bool useStereo = false;    // stereo kept (stereoInFile && stereo setting on)
    bool headerDirty = false;  // length was repaired; header must be rewritten
};

// Header block (header + body + CRC), exactly 0x188 bytes. The stale RAM
// pointer at body 0x28 is written as 0.
void encodePtiHeader(const Instrument& ins, uint8_t out[pti::kHeaderBlockSize]);

// Parse header + body. Requires type == 1 (spec 02 §10.4). A trailing ".wav"
// is stripped from the name. Bytes missing from a short file read as 0
// (spec 10 §0.1); structure < 9 runs the spec 10 §3.2 chain. The CRC is
// not computed (spec 02 §2).
FmtStatus decodePtiHeader(const uint8_t* file, size_t len, Instrument& ins, PtiLoadInfo* info = nullptr);

// spec 02 §6.3 payload rules. payloadBytes = fileSize - dataOffset.
PtiSampleLayout resolvePtiSample(uint32_t headerLengthFrames, uint32_t payloadBytes, bool stereoEnabled);

// Whole-file helpers (host tools / tests). `left`/`right` are frames of
// ins.lengthFrames each; right == nullptr writes mono. Returns bytes written,
// 0 if cap is too small.
size_t encodePti(const Instrument& ins, const int16_t* left, const int16_t* right, uint8_t* out, size_t cap);

// Size of the file encodePti would produce.
inline size_t ptiFileSize(uint32_t frames, bool stereo) {
    return pti::kDataOff + size_t(frames) * (stereo ? 4 : 2);
}

// Copy the payload into `dst` (planar: L block then, if layout.useStereo,
// R block). dstCapSamples is in int16 units. Updates ins.lengthFrames and
// ins.stereo from the layout.
FmtStatus decodePtiSamples(const uint8_t* file, size_t len, const PtiLoadInfo& info,
                           const PtiSampleLayout& layout, Instrument& ins, int16_t* dst,
                           size_t dstCapSamples);

// Override for the built-in spec 10 §3.2 chain (tests); called with the
// body bytes when structure < 9. nullptr = built-in chain.
void setPtiUpgradeHook(BlockUpgradeFn fn);

}  // namespace ot
