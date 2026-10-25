// Compile-time feature detection for the 2.x test songs.
//
// Each song needs a milestone of the 2.x line (docs/PLAN_2X.md). The generator
// is built against whatever src/core currently provides and only offers the
// songs the build can play. Nothing here has to change when a milestone lands
// as long as the milestone sets the marker named below; until then the hook
// stays a TODO in testsongs.cpp.
#pragma once

#include "app/app.h"
#include "model/constants.h"

namespace ts {

// M1 (docs/M1_PLAN.md §1): the working Pattern grows to 16 tracks. Pattern
// data for tracks 13-16 is written through App::pattern() like tracks 1-12
// and saved in the Tracker Mini 16-track pattern files (docs/specs/21 §4).
constexpr bool kHas16Tracks = ot::kTrackCount >= 16;

// M2 stereo playback (docs/specs/2x-04): audio/voice.h defines
// OT_FEATURE_STEREO_PLAYBACK (one-shot, loop, slice and beat-slice voices
// play both channels).
#if defined(OT_FEATURE_STEREO_PLAYBACK)
constexpr bool kHasStereo = true;
#else
constexpr bool kHasStereo = false;
#endif

// M3 synth framework and engines (docs/MINI_PLAN.md §3): audio/synth_bank.h
// defines OT_FEATURE_SYNTHS (three synth slots set up from code: model,
// patch, polyphony; saved in project.mt + N.patch; ACD / VAP / PERC play).
#if defined(OT_FEATURE_SYNTHS)
constexpr bool kHasSynths = true;
#else
constexpr bool kHasSynths = false;
#endif

// Synth k (1..3) is the step instrument byte kSynthBase + k - 1 (spec 21
// §4.1: 64..66).
constexpr int kSynthBase = 64;

}  // namespace ts
