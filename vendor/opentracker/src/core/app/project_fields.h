// Project values-block fields used by the UI (file offsets, spec 02 §3.3).
//
// spec 06 gives most of these as RAM addresses; the RAM mirror of file
// offset f is 0x20004780 + f (spec 02 §3.3), and "project+X" in spec 06 §4
// is read as base 0x200001cc + 0x4000 + X where that lands on a listed
// values-block field. Fields tagged [I] are OpenTracker's reading of those
// addresses; spec 02 does not name them.
#pragma once

#include <cstdint>

namespace ot {
namespace pf {
constexpr uint32_t kLastNote = 0x110;          // 0x20004890 last entered note
constexpr uint32_t kCurInstrument = 0x111;     // 0x20004891
constexpr uint32_t kLastFxType = 0x113;        // 0x20004893 last Fx-type index
constexpr uint32_t kCurPattern = 0x114;        // 0x20004894
constexpr uint32_t kScale = 0x115;             // 0x20004895 (spec 05 §4.1)
constexpr uint32_t kNotesLayout = 0x116;       // 0x20004896
constexpr uint32_t kRootNote = 0x117;          // 0x20004897
constexpr uint32_t kDelayFeedback = 0x11A;     // spec 06 §4.3 (project+0x6ce) 0..100
constexpr uint32_t kDelayTime = 0x11C;         // u16 ms 1..3500
constexpr uint32_t kDelayFlags = 0x11E;        // bit7 ping-pong, bit6 sync, low 6 bits rate
constexpr uint32_t kLimAttack = 0x120;         // u16 1..1000
constexpr uint32_t kLimRelease = 0x124;        // f32
constexpr uint32_t kLimThreshold = 0x128;      // i16 0..32767
constexpr uint32_t kStepJump = 0x12B;          // 0x200048ab 0..32
constexpr uint32_t kMute0 = 0x12C;             // u8[8] mute tracks 0..7 (spec 03 §2.4)
// Sample Recorder settings (spec 02 §3.3, spec 05 appendix C §2.1: project
// struct 0x200041cc + 0x6e8.. = file 0x134..; 0x13C radio frequency).
constexpr uint32_t kRecSource = 0x134;         // 0..3
constexpr uint32_t kRecGainLine = 0x135;       // 0..100
constexpr uint32_t kRecGainMicLow = 0x136;
constexpr uint32_t kRecGainMicHigh = 0x137;
constexpr uint32_t kRecGainRadio = 0x138;
constexpr uint32_t kRecMonitor = 0x139;        // 0 on, 1 off (spec 22 §5)
constexpr uint32_t kRadioFreq = 0x13C;         // f32 MHz
// Performance (spec 12 §A.2, §A.4): per-track patterns and the per-project
// preset source.
constexpr uint32_t kPerfPatterns = 0x142;      // u16[8] 1..255
constexpr uint32_t kPerfTypes = 0x152;         // u8[12]
constexpr uint32_t kPerfValues = 0x15E;        // i16[12][4]
constexpr uint32_t kPerfRows = 0x2F5;          // u8[12] (spec 12 §A.1 correction)
constexpr uint32_t kPerfReserved = 0x301;      // u8[8]
constexpr uint32_t kTempo = 0x1C0;             // f32
constexpr uint32_t kLineInLevel = 0x309;       // [I] project+0x8bd
constexpr uint32_t kLineInMute = 0x30A;        // [I]
constexpr uint32_t kLineInDelay = 0x30B;       // [I]
constexpr uint32_t kLineInReverb = 0x30D;      // [I]
constexpr uint32_t kReverbSize = 0x418;        // f32 0..1
constexpr uint32_t kReverbDamp = 0x41C;
constexpr uint32_t kReverbPreDelay = 0x420;
constexpr uint32_t kReverbDiffusion = 0x424;
constexpr uint32_t kTrackVolume = 0x4D0;       // u8[8] 0..100 (project+0x4a84)
constexpr uint32_t kMixReverb = 0x4D8;         // [I] global mixer levels / mutes
constexpr uint32_t kMixDelay = 0x4D9;
constexpr uint32_t kMixReverbMute = 0x4DA;
constexpr uint32_t kMixDelayMute = 0x4DB;
constexpr uint32_t kMixDryMute = 0x4DC;
constexpr uint32_t kMixDry = 0x4DD;
constexpr uint32_t kEqGain = 0x4E0;            // f32[5] -1..1 (project+0x4a94)
constexpr uint32_t kBassBoost = 0x4F4;         // 0..100
constexpr uint32_t kSpace = 0x4F5;             // 0..8
constexpr uint32_t kPatternDivider = 0x4F8;    // 0x20004c78 0..15
constexpr uint32_t kAntiAlias = 0x4F9;
constexpr uint32_t kLimiterMode = 0x4FA;
constexpr uint32_t kSidechain = 0x4FB;         // [I] 0..10
constexpr uint32_t kSendsMode = 0x4FC;
constexpr uint32_t kDelayFilter = 0x5FE;       // u16 100..20000
constexpr uint32_t kExtraHeadroom = 0x600;
constexpr uint32_t kSaturation = 0x601;        // 0..4
constexpr uint32_t kSnapToZero = 0x602;
constexpr uint32_t kMute8 = 0x742;             // u8[4] mute tracks 8..11
}  // namespace pf
}  // namespace ot
