// ACD synth engine (2.x WP-F, docs/MINI_PLAN.md §3; spec 21 §8.2 parameter
// table; Tracker Mini manual p.146, p.154) [OT clean-room DSP].
//
// A single-oscillator mono/poly "acid" voice:
//   oscillator  saw + pulse (PW, PW LFO, PW Env) + sub (-1 / -2 oct square,
//               pulse 25 %, triangle) + white noise; PolyBLEP, no
//               oversampling (OPTIMIZATION_2X item 10)
//   filter      LP SVF 12 dB (one TPT state-variable stage), LP SVF 24 dB
//               (two cascaded TPT stages, resonance on the second) or LP RD3
//               (4-pole TPT ladder cascade, one tanh on the feedback sum, no
//               Newton iterations); cutoff from Cutoff x key tracking x Mod
//               Env x LFO
//   amp         ADSR (Amp Env), Volume left to the framework
//   modulation  Mod Env (PW Env, Filter Env Amt), one LFO (6 shapes, Free /
//               One Shot / Note), patch glide (4 modes)
// Envelopes, LFO and every derived coefficient run at block rate; the voice
// ramps phase increment, pulse width, filter coefficients and amplitude
// linearly across the 128-frame block. No tables.
//
// Parameter semantics chosen here where the manual is silent (documented in
// acd.cpp next to the code): Pitch LFO in octaves (±3), filter envelope / LFO
// amounts on a square-root octave curve (±1 = ±10 octaves), Noise Mix sign
// selects pre-filter (+) or post-filter (-) noise, key tracking around C4.
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace acd {

// Parameter indices (spec 21 §8.2).
enum Param : int {
    kSawMix = 0,
    kSquareMix,
    kSubMix,
    kNoiseMix,
    kPw,
    kSubMode,
    kPwLfo,
    kPwEnv,
    kPitchLfo,
    kCutoff,
    kResonance,
    kFilterType,
    kNoteTrack,
    kFilterLfo,
    kFilterEnv,
    kAmpA,
    kAmpD,
    kAmpS,
    kAmpR,
    kModA,
    kModD,
    kModS,
    kModR,
    kLfoHz,
    kLfoWave,
    kLfoRetrig,
    kGlideMode,
    kGlideTime,
    kVolume,
    kParamCount   // 29
};

enum FilterType : int { kSvf12 = 0, kSvf24 = 1, kRd3 = 2 };
enum LfoWave : int { kTri = 0, kSine, kRampUp, kRampDown, kSquare, kSampleHold };
enum LfoRetrig : int { kFree = 0, kOneShot = 1, kNote = 2 };
enum GlideMode : int { kAlways = 0, kOverlap = 1, kLegato = 2, kLegatoOverlap = 3 };

}  // namespace acd

// The engine singleton (registered for kSynthEngineAcd in synth_registry.cpp).
ISynthEngine* synthAcdEngine();

}  // namespace audio
}  // namespace ot
