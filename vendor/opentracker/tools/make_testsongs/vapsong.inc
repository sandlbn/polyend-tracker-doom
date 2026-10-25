// Song 14 "OT Test 07 - VAP Only" (TESTSONGS.md). Included by testsongs.cpp
// inside its anonymous namespace (it uses Gen, setupSlot and setMacro).
//
// Only the VAP engine plays: Synth 1-3 are three VAP patches (instruments
// 64-66), our own, derived from the engine's built-in Init (no factory
// patch); no sample instrument, no other synth, no MIDI. 112 BPM, A minor.
//
// Synth 1 "OT VAP Pad" polyphony 4 (four-note chords, played alone),
// Synth 2 "OT VAP Lead" mono (osc sync, PWM, glide on overlapping notes),
// Synth 3 "OT VAP Bass" mono (Reset 1 / 2 On, filter envelope pluck). When
// the three play together the pad uses two tracks, so the held notes never
// pass the default budget of 4 voices.
//
// Levels: full notes carry no Volume FX; accents and ghosts are Volume 50
// and below (TESTSONGS.md Volume law). Every pattern ends its held pad notes
// with a note-off 12 steps early (the 0.9 s release ends before the next
// pattern), and every macro moved is set back on the last step.

namespace vaponly {

using namespace audio::vap;

constexpr float kTempoVap = 112.0f;
constexpr int kPad = kSynthBase + 0, kLead = kSynthBase + 1, kBass = kSynthBase + 2;
// Tracks (0-based): pad chord tones 1-4, bass, lead, a macro FX lane.
enum : int { kTP1 = 0, kTP2, kTP3, kTP4, kTBass, kTLeadV, kTLane };
constexpr int kMacroCentre = 63;   // bipolar macro: the patch as saved
const FxId kMacroFx[3] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC};

// Am7, Fmaj7, C, G (four voices, close voicings) and the bass roots.
constexpr int kChords[4][4] = {{55, 60, 64, 69}, {53, 57, 60, 64}, {55, 60, 64, 67}, {55, 59, 62, 67}};
constexpr int kRoots[4] = {33, 29, 36, 31};

void setupPatches(Gen& g) {
    // Pad: saw + detuned saw, slow attack, LP OB 24 with a tempo-synced LFO 1
    // on the cutoff; Reset Off (free-running phase). M1 Cutoff, M2
    // Resonance, M3 LFO depth.
    setupSlot(g, 0, SynthModel::VAP, "OT VAP Pad",
              {{kShape2, 0.5f}, {kDetune, 22.0f}, {kOscMix, 0.0f}, {kFilterType, 1.0f}, {kCutoff, 1600.0f},
               {kResonance, 0.35f}, {kFilterEnvAmt, 0.2f}, {kAmpA, 0.3f}, {kAmpD, 1.0f}, {kAmpS, 0.8f},
               {kAmpR, 0.9f}, {kLfo1Sync, 1.0f}, {kLfo1Ratio, 6.0f}, {kModBase + 0, 3.0f},
               {kModBase + 1, 3.0f}, {kModBase + 2, 0.15f}},
              4, -9.0f, 0.0f, 10.0f, 35.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Resonance", kResonance, 0.5f);
                  setMacro(p, 2, "LFO Depth", kModBase + 2, 0.1f);
              });
    // Lead: pulse osc 1 with LFO 2 PWM, saw osc 2 hard-synced to osc 1 and
    // swept by Env 2 (the sync "zap"), LP SVF 12; Glide Overlap: only notes
    // that overlap the previous one glide.
    setupSlot(g, 1, SynthModel::VAP, "OT VAP Lead",
              {{kShape1, 1.0f}, {kShape2, 0.5f}, {kPw1, 0.1f}, {kOscMix, 0.3f}, {kOscSync, 1.0f},
               {kFilterType, 4.0f}, {kCutoff, 3500.0f}, {kResonance, 0.3f}, {kAmpA, 0.005f}, {kAmpD, 0.5f},
               {kAmpS, 0.8f}, {kAmpR, 0.25f}, {kEnv2A, 0.0f}, {kEnv2D, 0.35f}, {kEnv2S, 0.2f}, {kEnv2R, 0.3f},
               {kLfo2Ratio, 5.0f}, {kLfo2Wave, 1.0f}, {kLfo2Sync, 0.0f}, {kGlideMode, 1.0f},
               {kGlideTime, 0.12f}, {kModBase + 0, float(kSrcEnv2)}, {kModBase + 1, float(kDstPitch2)},
               {kModBase + 2, 1.0f}, {kModBase + 3, float(kSrcLfo2)}, {kModBase + 4, float(kDstPw1)},
               {kModBase + 5, 0.3f}},
              1, -6.0f, 0.0f, 25.0f, 15.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Sync Sweep", kModBase + 2, 0.5f, true);
              });
    // Bass: square + saw an octave down, Reset 1 / 2 On (every note starts at
    // phase 0), LP MG 24 with a short filter envelope.
    setupSlot(g, 2, SynthModel::VAP, "OT VAP Bass",
              {{kShape1, 1.0f}, {kShape2, 0.5f}, {kTune2, -12.0f}, {kOscMix, -0.3f}, {kReset1, 1.0f},
               {kReset2, 1.0f}, {kFilterType, 0.0f}, {kCutoff, 300.0f}, {kResonance, 0.6f},
               {kFilterEnvAmt, 0.5f}, {kNoteTrack, 0.5f}, {kEnv1A, 0.0f}, {kEnv1D, 0.2f}, {kEnv1S, 0.0f},
               {kEnv1R, 0.1f}, {kAmpA, 0.0f}, {kAmpD, 0.3f}, {kAmpS, 0.5f}, {kAmpR, 0.08f}},
              1, -6.0f, 0.0f, 0.0f, 0.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Env Amt", kFilterEnvAmt, 0.4f, true);
              });
}

// Global mixer: delay 3/16 ping-pong, a medium room.
void vapMixer(Gen& g) {
    Project& p = g.app.project;
    p.setU8(pf::kMixReverb, 80);
    p.setU8(pf::kMixDelay, 70);
    p.setU8(pf::kDelayFeedback, 40);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempoVap * 0.75f));
    p.setU8(pf::kDelayFlags, uint8_t(p.u8(pf::kDelayFlags) | 0x80));
    p.setF32(pf::kReverbSize, 0.7f);
    p.setF32(pf::kReverbDamp, 0.4f);
    g.app.markProjectDirty();
}

void n(Gen& g, int t, int s, int note, int instr, int vol = -1) {
    g.note(t, s, note, instr, vol >= 0 ? FxSet{FxId::Volume, vol > 50 ? 50 : vol} : FxSet{});
}
// Macro FX on the lane (FX-only step; the step's instrument names the slot).
void macroAt(Gen& g, int s, int col, int macro, int v, int instr) {
    Step* st = g.step(kTLane, s);
    if (!st) return;
    st->instrument = uint8_t(instr);
    g.fx(kTLane, s, col, kMacroFx[macro], v);
}
void macroRamp(Gen& g, int s0, int s1, int col, int macro, int v0, int v1, int instr) {
    for (int s = s0; s <= s1; ++s) macroAt(g, s, col, macro, v0 + (v1 - v0) * (s - s0) / (s1 - s0), instr);
}
// Four-note chord of bar `bar` on tracks 1-4 at step s.
void chord4(Gen& g, int s, int bar) {
    for (int k = 0; k < 4; ++k) n(g, kTP1 + k, s, kChords[bar % 4][k], kPad);
}
void padOff(Gen& g, int s, int tracks = 4) {
    for (int k = 0; k < tracks; ++k) g.off(kTP1 + k, s);
}
// Bass bar: 8ths with a 16th pickup and an octave jump.
void bassBar(Gen& g, int b0, int bar, bool accents) {
    const int r = kRoots[bar % 4];
    const int steps[7] = {0, 3, 6, 8, 10, 12, 14};
    for (int k = 0; k < 7; ++k) {
        const int note = (k == 4 || k == 6) ? r + 12 : r;
        n(g, kTBass, b0 + steps[k], note, kBass, !accents || k == 0 ? -1 : (k % 2 ? 40 : 50));
    }
}
// The lead phrase over two bars (A minor pentatonic); `legato` = every
// note held into the next (they glide), else a note-off before each note.
void leadPhrase(Gen& g, int b0, bool legato) {
    const int s[8] = {0, 4, 6, 8, 12, 16, 22, 24};
    const int p[8] = {76, 79, 76, 74, 72, 69, 72, 64};
    for (int k = 0; k < 8; ++k) {
        n(g, kTLeadV, b0 + s[k], p[k], kLead);
        if (!legato) g.off(kTLeadV, b0 + s[k] + 1 + (k < 7 ? (s[k + 1] - s[k]) / 2 : 3));
    }
}

// 1 "Pad chords": four-note chords, one per bar, alone.
void chordsPattern(Gen& g) {
    g.beginPattern(1, 64, "Pad chords");
    for (int bar = 0; bar < 4; ++bar) chord4(g, bar * kBar, bar);
    padOff(g, 52);
    g.endPattern();
}

// 2 "Bass": the bass alone; M1 Cutoff up and down, M2 Env Amt in bar 4.
void bassPattern(Gen& g) {
    g.beginPattern(2, 64, "Bass");
    for (int bar = 0; bar < 4; ++bar) bassBar(g, bar * kBar, bar, bar >= 2);
    macroRamp(g, 0, 15, 0, 0, kMacroCentre, 127, kBass);
    macroRamp(g, 16, 31, 0, 0, 127, 0, kBass);
    macroAt(g, 32, 0, 0, kMacroCentre, kBass);
    macroRamp(g, 48, 62, 0, 1, 0, 127, kBass);
    macroAt(g, 63, 0, 1, kMacroCentre, kBass);
    g.off(kTBass, 63);
    g.endPattern();
}

// 3 "Lead glide": the phrase legato (glides), then detached (no glide).
void leadPattern(Gen& g) {
    g.beginPattern(3, 64, "Lead glide");
    leadPhrase(g, 0, true);
    leadPhrase(g, 2 * kBar, false);
    g.off(kTLeadV, 63);
    g.endPattern();
}

// 4 "Phase": the same note repeated, lead (Reset Off, free-running phase)
// in bars 1-2, bass (Reset On) in bars 3-4. Every note should sound alike.
void phasePattern(Gen& g) {
    g.beginPattern(4, 64, "Phase");
    for (int s = 0; s < 2 * kBar; s += 2) {
        n(g, kTLeadV, s, 69, kLead, s % 8 ? 40 : -1);
        g.off(kTLeadV, s + 1);
    }
    for (int s = 2 * kBar; s < 4 * kBar; s += 2) n(g, kTBass, s, 33, kBass, s % 8 ? 40 : -1);
    g.off(kTBass, 63);
    g.endPattern();
}

// 5 "Groove": all three; the pad plays two-note shells on tracks 1-2.
void groovePattern(Gen& g) {
    g.beginPattern(5, 64, "Groove");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        n(g, kTP1, b0, kChords[bar][1], kPad, 45);
        n(g, kTP2, b0, kChords[bar][3], kPad, 45);
        bassBar(g, b0, bar, true);
    }
    leadPhrase(g, 0, true);
    leadPhrase(g, 2 * kBar, false);
    padOff(g, 52, 2);
    g.off(kTBass, 63);
    g.off(kTLeadV, 63);
    g.endPattern();
}

// 6 "Pad macros": the chords with M1 Cutoff (bars 1-2), M2 Resonance (bar
// 3) and M3 LFO depth (bar 4) moving.
void padMacroPattern(Gen& g) {
    g.beginPattern(6, 64, "Pad macros");
    for (int bar = 0; bar < 4; ++bar) chord4(g, bar * kBar, bar);
    macroRamp(g, 0, 15, 0, 0, 0, 127, kPad);
    macroRamp(g, 16, 31, 0, 0, 127, kMacroCentre, kPad);
    macroRamp(g, 32, 47, 0, 1, 0, 127, kPad);
    macroAt(g, 48, 0, 1, 0, kPad);
    macroRamp(g, 48, 62, 0, 2, 0, 127, kPad);
    macroAt(g, 63, 0, 2, 0, kPad);
    padOff(g, 52);
    g.endPattern();
}

// 7 "Ending": one Am7 chord held a bar, then the release and the room.
void endingPattern(Gen& g) {
    g.beginPattern(7, 32, "Ending");
    chord4(g, 0, 0);
    padOff(g, 16);
    g.endPattern();
}

bool vapOnlySong(Gen& g) {
    g.tempo = kTempoVap;
    if (!g.begin(false)) return false;
    setupPatches(g);
    if (!g.ok) return false;
    trackNames(g, {"Pad 1", "Pad 2", "Pad 3", "Pad 4", "Bass", "Lead", "Macros"});
    chordsPattern(g);
    bassPattern(g);
    leadPattern(g);
    phasePattern(g);
    groovePattern(g);
    padMacroPattern(g);
    endingPattern(g);
    g.playlist({1, 2, 3, 4, 5, 6, 5, 7});
    vapMixer(g);
    return g.ok;
}

}  // namespace vaponly
