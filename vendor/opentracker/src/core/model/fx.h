// Step FX ids, names and value ranges (spec 02 §4.3).
//
// Names/symbols/ranges originate from Polyend tracker-lib types/patterns.ts
// (MIT, see third_party/LICENSES/tracker-lib.txt) as tabulated in spec 02.
#pragma once

#include <cstdint>

namespace ot {

enum class FxId : uint8_t {
    None = 0,
    Off = 1,
    MicroMove = 2,
    Roll = 3,
    Chance = 4,
    RandomNote = 5,
    RandomInstrument = 6,
    RandomVolume = 7,
    MidiCcA = 8,
    MidiCcB = 9,
    MidiCcC = 10,
    MidiCcD = 11,
    MidiCcE = 12,
    BreakPattern = 13,
    MidiChord = 14,
    Tempo = 15,
    RandomFxValue = 16,
    Swing = 17,
    Volume = 18,
    Glide = 19,
    GateLength = 20,
    Arp = 21,
    Position = 22,
    VolumeLfo = 23,
    PanningLfo = 24,
    Slice = 25,
    ReversePlayback = 26,
    LowPass = 27,
    HighPass = 28,
    BandPass = 29,
    DelaySend = 30,
    Panning = 31,
    ReverbSend = 32,
    FinetuneLfo = 33,
    MicroTune = 34,
    FilterLfo = 35,
    PositionLfo = 36,
    MidiCcF = 37,
    Overdrive = 38,
    BitDepth = 39,
    Tune = 40,
    SlideUp = 41,
    SlideDown = 42,
};

constexpr int kFxCount = 43;

struct FxInfo {
    char symbol;          // single-character column symbol
    const char* name;     // menu name
    uint8_t minValue;     // stored-value range (inclusive)
    uint8_t maxValue;
    uint8_t defaultValue;
};

// Info for a stored FX id; ids >= kFxCount return the "None" entry.
const FxInfo& fxInfo(uint8_t id);
inline const FxInfo& fxInfo(FxId id) { return fxInfo(static_cast<uint8_t>(id)); }
inline bool fxIdValid(uint8_t id) { return id < kFxCount; }

// Firmware menu (display) order of ids, spec 02 §4.3.
extern const uint8_t kFxMenuOrder[kFxCount];

}  // namespace ot
