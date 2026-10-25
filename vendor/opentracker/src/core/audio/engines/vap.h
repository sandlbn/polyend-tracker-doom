// VAP: virtual analog poly synth engine (Tracker Mini engine id 4; spec 21
// §8.4 with the MINI_PLAN corrections, Mini manual p.157-160) [OT].
//
// Clean-room DSP of our own design that follows the VAP parameter set
// (docs/OPTIMIZATION_2X.md item 10 budget: <= 12k M4 cycles per voice per
// block for the Init patch, <= 18k worst case, no oversampling, block-rate
// modulation, no shared tables):
//
//  - 2 oscillators: one phase each, shape morph triangle (0) -> saw (0.5) ->
//    pulse (1); the pulse is saw(phase) - saw(phase - width), so saw / pulse
//    cost 1-2 polyBLEP corrections and the triangle 2 polyBLAMP corrections.
//    Hard sync (Osc Sync) resets osc 2 on osc 1 wraps with a BLEP of the
//    jump. Noise is a 32-bit LCG.
//  - Filter: TPT state-variable stages (LP / HP / BP / notch); 24 dB = two
//    cascaded stages, the resonance in the second. Flavours: SVF clean with
//    a little resonance bass loss, OB with a soft clip after the filter, MG
//    (LP 24 only) a ladder-like response: two coincident poles in the first
//    stage, a driven input and more bass loss with resonance.
//  - Amp Env, Env 1 (filter), Env 2 (wave / osc mix), 2 LFOs and the 6-slot
//    mod matrix run at block rate; every destination is evaluated at the
//    block start and end and ramped linearly across the 128 frames.
//  - Glide: exponential portamento of the note pitch (Glide Mode / Time).
//
// Engine contract (audio/synth_engine.h): Volume (index 39) is never applied
// here; output is mono (L = R).
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace vap {

// Parameter indices (spec 21 §8.4).
enum Param : int {
    kShape1 = 0, kShape2, kPw1, kPw2, kOscMix, kTune1, kTune2, kDetune, kFinetune, kReset1, kReset2,
    kFilterType = 11, kCutoff, kResonance, kFilterEnvAmt,
    kAmpA = 15, kAmpD, kAmpS, kAmpR,
    kEnv1A = 19, kEnv1D, kEnv1S, kEnv1R,
    kEnv2A = 23, kEnv2D, kEnv2S, kEnv2R,
    kLfo1Ratio = 27, kLfo1Wave, kLfo1Retrig, kLfo1Sync,
    kLfo2Ratio = 31, kLfo2Wave, kLfo2Retrig, kLfo2Sync,
    kGlideMode = 35, kGlideTime, kNoteTrack, kMixEnvAmt, kVolume, kOscSync, kNoise,
    kModBase = 42,   // slot k: Src 42+3k, Dest 43+3k, Amt 44+3k
    kParamCount = 60,
};

// Mod matrix sources / destinations (Mini manual p.159).
enum ModSrc : int { kSrcAmpEnv = 0, kSrcEnv1, kSrcEnv2, kSrcLfo1, kSrcLfo2, kSrcCount };
enum ModDest : int {
    kDstPw1 = 0, kDstPw2, kDstOscMix, kDstCutoff, kDstAmpSus, kDstEnv1Sus, kDstEnv2Sus, kDstVolume,
    kDstPitch1, kDstPitch2, kDstEnv1Decay, kDstNoise, kDstShape1, kDstShape2, kDstCount
};

ISynthEngine* engine();

// Tempo for LFO Sync (Ratio = tempo-division index 0..15 when Sync is On).
// The framework sets it every block from the song tempo through
// ISynthEngine::setTempo; 120 BPM until the first block.
void setTempo(float bpm);

}  // namespace vap
}  // namespace audio
}  // namespace ot
