// Sample-instrument parameter access shared by the Instrument screen and the
// MIDI CC map (spec 05 appendix C §4-§6, §8). Both paths go through the
// same clamps, so a CC edit lands exactly where an encoder edit would.
//
// Every parameter is handled in integer "knob units":
//   Volume 0..100, Panning 0..100 (50 centre), Tune -24..24, Finetune
//   -100..100, Filter type 0..3 (0 disabled, 1 LP, 2 HP, 3 BP), Cutoff
//   0..100 (x0.01), Resonance 0..100 (x0.043), Overdrive 0..100, Bit depth
//   4..16, Reverb/Delay send 0..100; automation (per destination 0..5):
//   Type 0..2 (off/envelope/LFO), Attack|Decay|Release knob 0..190 (ms knob,
//   §6), LFO shape 0..4, LFO speed 0..28 (0..23 for Volume), Sustain and
//   Amount 0..100 (x0.01).
#pragma once

#include <cstdint>

#include "model/instrument.h"

namespace ot {

enum class InstrParam : uint8_t {
    Volume,
    Panning,
    Tune,
    Finetune,
    FilterType,
    Cutoff,
    Resonance,
    Overdrive,
    BitDepth,
    ReverbSend,
    DelaySend,
    // Automation (destination given separately). The 6-CC block order of
    // appendix C §8: type, attack|LFO shape, decay|LFO speed, sustain,
    // release, amount.
    AutoType,
    AutoAttackShape,
    AutoDecaySpeed,
    AutoSustain,
    AutoRelease,
    AutoAmount,
};

namespace iedit {

struct Range {
    int lo, hi;
};

// Envelope time knob <-> milliseconds (appendix C §6).
int msToKnob(int ms);
int knobToMs(int knob);

// Automation type of destination `dest`: 0 off, 1 envelope, 2 LFO.
int autoType(const Instrument& in, int dest);

Range range(const Instrument& in, InstrParam p, int dest = 0);
int get(const Instrument& in, InstrParam p, int dest = 0);
// False when the parameter is inactive in the current state: Cutoff and
// Resonance while the filter is disabled (§4 col5/6); automation fields
// other than Type while the type is Off; envelope-only fields with an LFO
// and LFO-only fields with an envelope (§6).
bool editable(const Instrument& in, InstrParam p, int dest = 0);
// Clamp `units` into range and store. Returns true when the value changed.
bool set(Instrument& in, InstrParam p, int dest, int units);
// Step by `delta`; with `wrap` (button tap on a list column) values past
// either end wrap around instead of clamping (§3).
bool step(Instrument& in, InstrParam p, int dest, int delta, bool wrap = false);
// Default value (reset key, §4/§5/§6 reset tables).
int defaultValue(InstrParam p, int dest = 0);

}  // namespace iedit
}  // namespace ot
