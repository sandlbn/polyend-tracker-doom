// PERC drum machine engine (2.x WP-F; spec 21 §8.6 with the MINI_PLAN
// corrections; Tracker Mini manual p.146, p.166-172) [OT].
//
// Clean-room design: only the parameter semantics follow the Mini (stored
// ranges, the drum per note, the mixer and FX block). Every DSP algorithm
// here is our own and is an APPROXIMATION of the described sounds:
//  - "Body Wave" 0..175 = 16 wave families x 11 positions (0 %, 10 %, ..
//    100 %), index = family * 11 + position. Each family starts at a sine
//    (0 %) and morphs into its own shape, computed per sample from a
//    parabolic sine (no tables): Sat-Sin (rational soft clip), Triangle,
//    Square, Square FM, FM1/3/7/11 (phase modulation by the 1/3/7/11th
//    harmonic), tzFM1/3/7/11 (phase modulation by a squared modulator, a
//    stand-in for through-zero FM), Glitch 1..4 (phase quantise, amplitude
//    crush, wavefold, phase-bit XOR).
//  - Kick / Tom / Snare / Hi-hat / Cymbal / Perc models, the hat and cymbal
//    "Timbre" 0..24 (square-bank spread or one of 25 FM presets), the
//    waveshaper types and the 3-band EQ (one-pole shelves + a TPT band) are
//    our own designs that keep the manual's description.
//
// Notes (MINI_PLAN U3): Kick 60, Snare 62, Hi-hat 64..67 (64 closed .. 67
// open, decay interpolated geometrically between Closed and Open Decay),
// Cymbal 69, Perc 71. 61, 63, 68 and 70 play nothing. Every other note plays
// the Tom at the note's pitch.
//
// Voices and state: PERC is one voice per slot, but its drums overlap: the
// whole kit (six drum channels, the FX state) lives in the slot scratch
// (SynthSlotScratch, 384 B; slotStateBytes() used), the voice keeps only an
// ownership token (8 B). A note retriggers its own drum channel and leaves
// the others ringing (the hi-hat notes share one channel: closed chokes
// open). A new voice of the same slot (polyphony > 1) takes the kit over;
// the old voice then ends silently. The voice ends when every drum is silent
// and is freed at once when the framework fades it out (steal).
//
// Velocity (Macro 7) applies per drum hit, in the engine: the framework's
// voice gain for PERC is the LAST note's velocity, which would also scale
// the drums still ringing. The engine divides it back out: the ratio gain /
// last velocity is a kit master, 1 after a hit, moved by a Volume FX on an
// empty step (it scales the ringing kit; the next hit folds it into the
// drums it scaled). A stolen / cut voice (SynthVoice::fading, set by the
// framework) ramps the master to 0 over its last block and frees the kit,
// whatever the last velocity was. Volume is never applied.
//
// Per-drum sends: render() has no send outputs, so the per-drum Delay /
// Reverb sends go to a per-slot tap (setSendTap). The framework
// (SynthBank::render, ISynthEngine::setSendTap) points it at the main mono
// send buses every block, with the slot gain x Global Mixer level as the bus
// gains, and clears it after the slot's voices. Without a tap they are
// ignored.
//
// Cost (OPTIMIZATION_2X item 10: <= 30k M4 cycles per slot per block):
// block-rate coefficients (fast exp2 / log2 / tan, no libm, no tables), a
// parabolic sine of a fixed-point phase, block-linear envelopes folded into
// the mix, tonal bodies at half rate below 5 kHz, packed 16-bit square
// banks, FX loops specialised per type / EQ band set. On top, a cost
// governor: at most five drums ring at once (the manual's "up to five drum
// sounds at once"), and a hit (or a macro move) that would take the kit's
// estimated cost (kitCost: steady drum and FX costs, the send tap, and the
// kick click / tom noise while they last) over kKitBudget - 1k cuts the
// quietest other drums. The 1k headroom covers the note-on work of the hit
// block. Measured peak block (audio_bench perc.pk, all six drums hit in one
// block, Init / costly / send-tap / dry kits): 29.4k.
#pragma once

#include <cstdint>

#include "audio/synth_engine.h"

namespace ot {
namespace audio {

ISynthEngine* percEngine();

namespace perc {

enum Drum : int8_t { kNone = -1, kKick = 0, kTom, kSnare, kHat, kCymbal, kPerc, kDrums };

// Drum played by a note (kNone: silent). hatVariant: 0 closed .. 3 open.
Drum drumForNote(uint8_t note, int* hatVariant = nullptr);

// Parameter indices (spec 21 §8.6).
enum Param : int {
    kKickWave = 0, kKickPitch, kKickAttack, kKickDecay, kKickTrTimbre, kKickTrLevel, kKickModDecay, kKickModAmount,
    kKickModel,
    // Tom 12 / 13: transient level (0..1) / timbre (0..2), the way round the
    // manual's ranges (p.166) fit the stored ranges.
    kTomWave = 9, kTomAttack, kTomDecay, kTomTrLevel, kTomTrTimbre, kTomModDecay, kTomPitchMod, kTomWaveMod, kTomModel,
    kSnareWave = 18, kSnarePitch, kSnareTone, kSnareModel, kSnareDecay, kSnareSnapAttack, kSnareSnapDecay, kSnareMix,
    kSnareSnapTimbre,
    kHatShape = 27, kHatClosed, kHatOpen, kHatHpf, kHatLpf, kHatPitch, kHatMix, kHatTimbre, kHatModel,
    kCymBodyF = 36, kCymImpactF, kCymBodyDecay, kCymImpactDecay, kCymPitch, kCymMix, kCymImpactBody, kCymModel,
    kCymTimbre,
    kPercModel = 45, kPercVariation, kPercTimbre, kPercDecay,
    kMixer = 49,   // + 5 * drum: Level, Pan, Delay, Reverb, Output
    kWsLevel = 79, kWsType, kEqLow, kEqMid, kEqHigh,
};
constexpr int kParamCount = 84;

// Per-slot send tap (see above): mono delay / reverb buses of kBlock floats
// that render() adds into, each drum's send x gainD / gainR. nullptr = none
// (the default).
void setSendTap(int slot, float* delay, float* reverb, float gainD = 1.0f, float gainR = 1.0f);

// Slot state size (tests / docs).
int slotStateBytes();
// Estimated M4 instructions per block of the slot's kit (the cost governor's
// table) and the number of ringing drums (tests / bench).
int32_t kitCost(SynthSlotScratch& slot, const SynthParams& p, int* ringing = nullptr);
constexpr int32_t kKitBudget = 30000;

}  // namespace perc
}  // namespace audio
}  // namespace ot
