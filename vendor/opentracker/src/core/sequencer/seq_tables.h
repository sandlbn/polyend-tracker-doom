// Sequencer constant tables (spec 03 §1, §2.6, §3.5, §3.6).
#pragma once

#include <cstdint>

namespace ot {
namespace seq {

// Timing resolution, spec 03 §0 / §6.
constexpr int kTicksPerMicro = 12;
constexpr int kMicrosPerStep = 48;
constexpr int kTicksPerStep = kTicksPerMicro * kMicrosPerStep;  // 576
constexpr int kMicrosPerClock = 8;                              // 24 PPQN
constexpr int kTicksPerExtClock = kTicksPerMicro * kMicrosPerClock;  // 96

// Real-time advance: elapsed = n / 44122.6484 s (spec 03 §1.1).
constexpr float kRealtimeRate = 44122.6484f;
constexpr uint32_t kBlockFrames = 128;
// Offline render step per 128-frame block (spec 03 §1.1).
constexpr float kRenderBlockSeconds = 0.0029013333f;

// Tempo (spec 03 §1.2).
constexpr float kTempoMin = 40.0f;      // UI clamp
constexpr float kTempoMax = 800.0f;
constexpr float kTempoDefault = 130.0f;
constexpr float kTempoFxMin = 5.0f;     // Tempo FX override clamp, spec 03 §3.6 id 15
constexpr float kTempoFxMax = 800.0f;

// Roll (id 3), spec 03 §3.5: period in microsteps per rate index 1..8.
extern const uint8_t kRollPeriod[8];
// Arp (id 21), spec 03 §3.5: period in microsteps per rate index 0..10.
extern const uint16_t kArpPeriod[11];
// Performance step repeater, spec 03 §2.6 (index 0..14).
extern const uint16_t kRepeatPeriod[15];

// MIDI Chord (id 14) interval sets, spec 03 §3.5 table. Up to 4 intervals,
// count in kChordSize. Value 0 = none.
constexpr int kChordCount = 30;
extern const uint8_t kChordIntervals[kChordCount][4];
extern const uint8_t kChordSize[kChordCount];
// 12-bit pitch-class mask of a chord (bit i = interval i mod 12).
uint16_t chordMask(uint8_t value);

// Engine-enforced FX value ranges (spec 03 §3.6 "Range"; differs from
// tracker-lib for several ids). Ids >= 43 return 0..255.
uint8_t fxMin(uint8_t type);
uint8_t fxMax(uint8_t type);

}  // namespace seq
}  // namespace ot
