// Song 16 "OT Test 09 - FAT Only" (TESTSONGS.md). Included by testsongs.cpp
// inside its anonymous namespace (it uses Gen, setupSlot and setMacro).
//
// Only the FAT engine plays (2.1): Synth 1-3 are three FAT patches
// (instruments 64-66), our own, derived from the engine's built-in Init (no
// factory patch); no sample instrument, no other synth, no MIDI. 128 BPM,
// A minor.
//
// Synth 1 "OT FAT Pad" polyphony 4 (a seven-saw pad, four-note chords),
// Synth 2 "OT FAT Lead" mono (full stack part pulse, vibrato, legato glide,
// Reset Off), Synth 3 "OT FAT Bass" mono (three saws, Reset On, MG filter
// envelope). When the three play together the pad uses two tracks, so the
// held notes never pass the default budget of 4 voices.
//
// Levels: full notes carry no Volume FX; accents and ghosts are Volume 50
// and below (TESTSONGS.md Volume law). Every pattern ends its held pad notes
// with a note-off 12 steps early (the release ends before the next pattern),
// and every macro moved is set back on the last step.

namespace fatonly {

using namespace audio::fat;

constexpr float kTempoFat = 128.0f;
constexpr int kPad = kSynthBase + 0, kLead = kSynthBase + 1, kBass = kSynthBase + 2;
// Tracks (0-based): pad chord tones 1-4, bass, lead, a macro FX lane.
enum : int { kTP1 = 0, kTP2, kTP3, kTP4, kTBass, kTLeadV, kTLane };
constexpr int kMacroCentre = 63;   // bipolar macro: the patch as saved
const FxId kMacroFx[3] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC};

// Am, F, C, G (four voices, close voicings) and the bass roots.
constexpr int kChords[4][4] = {{57, 60, 64, 69}, {53, 57, 60, 65}, {55, 60, 64, 67}, {55, 59, 62, 67}};
constexpr int kRoots[4] = {33, 29, 36, 31};

void setupPatches(Gen& g) {
    // Pad: the full stack, a slow attack, LP OB 24 with a soft filter
    // envelope. M1 Cutoff, M2 Fatness (crosses the pair thresholds), M3 Timbre.
    setupSlot(g, 0, SynthModel::FAT, "OT FAT Pad",
              {{kFatness, 0.8f}, {kBrightness, 0.6f}, {kOscLfo, 0.06f}, {kFilterType, float(kLpOb24)},
               {kCutoff, 2200.0f}, {kResonance, 0.3f}, {kFilterEnv, 0.15f}, {kEnvA, 0.3f}, {kEnvD, 1.0f},
               {kEnvS, 0.5f}, {kAmpA, 0.2f}, {kAmpD, 1.0f}, {kAmpS, 0.85f}, {kAmpR, 0.6f}, {kLfoHz, 5.0f}},
              4, -12.0f, 0.0f, 10.0f, 35.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Fatness", kFatness, 0.8f, true);
                  setMacro(p, 2, "Timbre", kTimbre, 1.0f);
              });
    // Lead: the full stack part pulse, deep vibrato, legato glide (notes
    // held into the next glide), Reset Off (free-running phases), LP OB 12.
    setupSlot(g, 1, SynthModel::FAT, "OT FAT Lead",
              {{kFatness, 1.0f}, {kTimbre, 0.4f}, {kBrightness, 0.6f}, {kOscLfo, 0.15f}, {kReset, 0.0f},
               {kFilterType, float(kLpOb12)}, {kCutoff, 2500.0f}, {kResonance, 0.5f}, {kFilterEnv, 0.2f},
               {kEnvD, 0.5f}, {kEnvS, 0.3f}, {kAmpA, 0.005f}, {kAmpS, 1.0f}, {kAmpR, 0.25f}, {kLfoHz, 5.5f},
               {kGlideMode, float(kLegato)}, {kGlideTime, 0.12f}},
              1, -6.0f, 0.0f, 25.0f, 15.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Vibrato", kOscLfo, 0.3f, true);
              });
    // Bass: three saws a little pulse, Reset On (every note starts at the
    // same phases), LP MG 24 with a short filter envelope, a breath of noise.
    setupSlot(g, 2, SynthModel::FAT, "OT FAT Bass",
              {{kFatness, 0.35f}, {kTimbre, 0.2f}, {kBrightness, 0.5f}, {kNoise, 0.03f}, {kOscLfo, 0.0f},
               {kFilterType, float(kLpMg24)}, {kCutoff, 260.0f}, {kResonance, 0.6f}, {kFilterEnv, 0.4f},
               {kEnvD, 0.25f}, {kEnvS, 0.0f}, {kAmpA, 0.002f}, {kAmpD, 0.4f}, {kAmpS, 0.6f}, {kAmpR, 0.08f}},
              1, -4.0f, 0.0f, 0.0f, 0.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", kCutoff, 0.3f, true);
                  setMacro(p, 1, "Env Amt", kFilterEnv, 0.4f, true);
              });
}

// Global mixer: delay 3/16 ping-pong, a medium room.
void fatMixer(Gen& g) {
    Project& p = g.app.project;
    p.setU8(pf::kMixReverb, 80);
    p.setU8(pf::kMixDelay, 70);
    p.setU8(pf::kDelayFeedback, 40);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempoFat * 0.75f));
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
// Bass bar: 8ths with an octave jump on the off-beats.
void bassBar(Gen& g, int b0, int bar, bool accents) {
    const int r = kRoots[bar % 4];
    for (int k = 0; k < 8; ++k) {
        const int note = k % 2 ? r + 12 : r;
        n(g, kTBass, b0 + 2 * k, note, kBass, !accents || k == 0 ? -1 : (k % 2 ? 40 : 50));
    }
}
// The lead phrase over two bars (A minor); `legato` = every note held into
// the next (they glide), else a note-off before each note.
void leadPhrase(Gen& g, int b0, bool legato) {
    const int s[8] = {0, 3, 6, 8, 12, 16, 20, 24};
    const int p[8] = {69, 72, 76, 74, 72, 71, 72, 64};
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

// 4 "Phase": the same note repeated, lead (Reset Off, free-running phases)
// in bars 1-2, bass (Reset On) in bars 3-4.
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

// 6 "Pad macros": the chords with M2 Fatness swept down and up through the
// pair thresholds (bars 1-2), M3 Timbre (bar 3) and M1 Cutoff (bar 4).
void padMacroPattern(Gen& g) {
    g.beginPattern(6, 64, "Pad macros");
    for (int bar = 0; bar < 4; ++bar) chord4(g, bar * kBar, bar);
    macroRamp(g, 0, 15, 0, 1, kMacroCentre, 0, kPad);
    macroRamp(g, 16, 31, 0, 1, 0, 127, kPad);
    macroAt(g, 32, 0, 1, kMacroCentre, kPad);
    macroRamp(g, 32, 47, 0, 2, 0, 127, kPad);
    macroAt(g, 48, 0, 2, 0, kPad);
    macroRamp(g, 48, 62, 0, 0, kMacroCentre, 127, kPad);
    macroAt(g, 63, 0, 0, kMacroCentre, kPad);
    padOff(g, 52);
    g.endPattern();
}

// 7 "Ending": one Am chord held a bar, then the release and the room.
void endingPattern(Gen& g) {
    g.beginPattern(7, 32, "Ending");
    chord4(g, 0, 0);
    padOff(g, 16);
    g.endPattern();
}

bool fatOnlySong(Gen& g) {
    g.tempo = kTempoFat;
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
    fatMixer(g);
    return g.ok;
}

}  // namespace fatonly
