// Song 13 "OT Test 06 - PERC Only" (TESTSONGS.md). Included by testsongs.cpp
// inside its anonymous namespace (it uses Gen and setupSlot from there).
//
// Only the PERC engine plays: Synth 1-3 are three PERC kits (instruments
// 64-66), our own patches derived from the engine's built-in Init (no
// factory patch); no sample instrument, no other synth, no MIDI. 120 BPM.
//
// PERC notes (audio/engines/perc.h, spec 21 §4.1, MINI_PLAN U3): kick 60,
// snare 62, hi-hat 64..67 (64 closed .. 67 open, one channel: a closed hat
// chokes an open one), cymbal 69, perc 71; 61 / 63 / 68 / 70 play nothing;
// every other note is the tom at that pitch (one tom channel per kit).
//
// Levels: on a synth step Volume is the velocity (0..100, macro 7) and a hit
// without a Volume FX plays at the patch level. The song never writes a
// Volume above 50 (TESTSONGS.md Volume law): full hits carry no Volume FX,
// accents / ghosts are Volume 50 and below. Kit gains -9 dB.
//
// Macros (the same six on every kit, all bipolar: 63 = the patch as saved):
// M1 Tone, M2 Decay, M3 Pitch, M4 Snap, M5 Drive, M6 Space. The sequencer
// keeps a macro FX value until the transport stops, so every pattern that
// moves a macro puts it back to 63 on its last step.

namespace perconly {

using namespace audio::perc;

constexpr float kTempoPerc = 120.0f;
constexpr int kA = kSynthBase + 0, kB = kSynthBase + 1, kC = kSynthBase + 2;   // kits = Synth 1..3
// Tracks (0-based): one drum family per track, a macro FX lane, kits B / C
// in the last pattern.
enum : int { kTK = 0, kTS, kTH, kTCym, kTP, kTTom, kTLane, kTKitB, kTKitC };
// Drum notes.
constexpr int nKick = 60, nSnare = 62, nHatC = 64, nHat1 = 65, nHat2 = 66, nHatO = 67, nCym = 69, nClap = 71;
constexpr int kMacroCentre = 63;

int lvl(int drum, int field) { return kMixer + 5 * drum + field; }   // field: 0 level, 1 pan, 2 delay, 3 reverb

// The six macros, the same targets on every kit (amount x stored range per
// unit of the bipolar macro, synth_params.h).
void kitMacros(SynthPatch& p) {
    struct T {
        int param;
        float amount;
    };
    auto macro = [&p](int k, const char* name, std::initializer_list<T> ts) {
        PatchMacro& m = p.macros[k];
        std::snprintf(m.name, sizeof(m.name), "%s", name);
        m.nameLen = uint8_t(std::strlen(m.name));
        m.bipolar = true;
        m.count = 0;
        for (const T& t : ts) {
            if (m.count >= kPatchMacroTargets) break;
            m.targets[m.count].param = t.param;
            m.targets[m.count].amountBits = patchFloatToBits(t.amount);
            ++m.count;
        }
    };
    macro(0, "Tone", {{kSnareTone, 0.4f}, {kHatLpf, 0.25f}, {kCymBodyF, 0.2f}, {kPercTimbre, 0.3f},
                      {kKickTrTimbre, 0.3f}, {kSnareSnapTimbre, 0.3f}});
    macro(1, "Decay", {{kKickDecay, 0.04f}, {kSnareDecay, 0.02f}, {kHatOpen, 0.05f}, {kCymBodyDecay, 0.1f},
                       {kTomDecay, 0.04f}, {kPercDecay, 0.15f}});
    macro(2, "Pitch", {{kKickPitch, 0.08f}, {kSnarePitch, 0.08f}, {kHatPitch, 0.2f}, {kCymPitch, 0.2f}});
    macro(3, "Snap", {{kSnareMix, 0.4f}, {kSnareSnapDecay, 0.02f}, {kKickTrLevel, 0.4f}, {kTomTrLevel, 0.4f}});
    macro(4, "Drive", {{kWsLevel, 0.4f}});
    macro(5, "Space", {{lvl(kSnare, 2), 0.4f}, {lvl(kSnare, 3), 0.4f}, {lvl(kHat, 2), 0.4f}, {lvl(kHat, 3), 0.3f}});
}

// Kit A "OT Perc Deep": sine kick with a long pitch drop (Classic9), Classic+
// toms, Classic9 snare, long open hat and cymbal, clap.
std::vector<ParamEdit> kitDeep() {
    return {{kKickWave, 0.0f},      {kKickPitch, 28.0f},       {kKickDecay, 0.6f},        {kKickModel, 2.0f},
            {kKickModAmount, 0.25f}, {kKickModDecay, 0.06f},   {kKickTrLevel, 0.4f},      {kKickTrTimbre, 0.3f},
            {kTomModel, 1.0f},      {kTomDecay, 0.5f},         {kTomPitchMod, 2.0f},      {kSnareModel, 1.0f},
            {kSnarePitch, 52.0f},   {kSnareTone, 0.45f},       {kSnareDecay, 0.2f},       {kSnareSnapDecay, 0.3f},
            {kSnareMix, -0.1f},     {kHatModel, 0.0f},         {kHatClosed, 0.08f},       {kHatOpen, 0.9f},
            {kCymBodyDecay, 2.0f},  {kPercModel, 0.0f},        {kPercDecay, 0.3f},        {lvl(kTom, 0), 0.8f},
            {lvl(kSnare, 2), 0.0f}, {lvl(kSnare, 3), 0.15f},   {lvl(kHat, 2), 0.0f},      {lvl(kCymbal, 2), 0.0f},
            {lvl(kCymbal, 3), 0.2f}, {lvl(kPerc, 2), 0.0f},    {lvl(kPerc, 3), 0.15f},    {kEqLow, 2.0f}};
}

// Kit B "OT Perc Tight": square-bodied kick with a noise click (Classic8),
// FM toms, Modern snare, FM hats and cymbal, cowbell, light waveshaper.
std::vector<ParamEdit> kitTight() {
    return {{kKickWave, 2 * 11 + 4.0f}, {kKickPitch, 33.0f},      {kKickDecay, 0.22f},     {kKickModel, 0.0f},
            {kKickModAmount, 0.5f},     {kKickModDecay, 0.03f},   {kKickTrTimbre, -0.4f},  {kKickTrLevel, 0.6f},
            {kTomWave, 4 * 11 + 3.0f},  {kTomModel, 0.0f},        {kTomDecay, 0.25f},      {kSnareModel, 3.0f},
            {kSnarePitch, 55.0f},       {kSnareDecay, 0.12f},     {kSnareSnapDecay, 0.15f}, {kSnareMix, 0.2f},
            {kSnareSnapTimbre, 0.5f},   {kHatModel, 1.0f},        {kHatTimbre, 7.0f},      {kHatClosed, 0.05f},
            {kHatOpen, 0.4f},           {kHatHpf, 7000.0f},       {kCymModel, 1.0f},       {kCymTimbre, 12.0f},
            {kCymBodyDecay, 1.0f},      {kPercModel, 4.0f},       {kPercTimbre, 0.55f},    {kPercDecay, 0.25f},
            {lvl(kSnare, 2), 0.0f},     {lvl(kHat, 2), 0.0f},     {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f},
            {kWsType, 1.0f},            {kWsLevel, 0.2f}};
}

// Kit C "OT Perc Space": glitch kick (Synced8), resonant tzFM toms, Modern+
// snare, shaker; delay and reverb sends on the snare and the hats.
std::vector<ParamEdit> kitSpace() {
    return {{kKickWave, 12 * 11 + 5.0f}, {kKickPitch, 30.0f},     {kKickDecay, 0.4f},      {kKickModel, 1.0f},
            {kTomWave, 8 * 11 + 4.0f},   {kTomModel, 2.0f},       {kTomDecay, 0.6f},       {kTomPitchMod, 4.0f},
            {kSnareModel, 4.0f},         {kSnarePitch, 48.0f},    {kSnareDecay, 0.18f},    {kSnareMix, -0.2f},
            {kHatClosed, 0.1f},          {kHatOpen, 0.6f},        {kHatTimbre, 3.0f},      {kHatLpf, 12000.0f},
            {kCymBodyDecay, 2.5f},       {kCymPitch, 300.0f},     {kPercModel, 3.0f},      {kPercVariation, 0.4f},
            {kPercDecay, 0.4f},          {lvl(kSnare, 2), 0.6f},  {lvl(kSnare, 3), 0.5f},  {lvl(kHat, 2), 0.5f},
            {lvl(kHat, 3), 0.3f},        {lvl(kCymbal, 2), 0.0f}, {lvl(kCymbal, 3), 0.4f}, {lvl(kPerc, 2), 0.4f},
            {kEqHigh, 3.0f},             {kEqLow, -2.0f}};
}

void setupKits(Gen& g) {
    setupSlot(g, 0, SynthModel::PERC, "OT Perc Deep", kitDeep(), 1, -9.0f, 0.0f, 0.0f, 10.0f, kitMacros);
    setupSlot(g, 1, SynthModel::PERC, "OT Perc Tight", kitTight(), 1, -9.0f, 0.0f, 0.0f, 0.0f, kitMacros);
    setupSlot(g, 2, SynthModel::PERC, "OT Perc Space", kitSpace(), 1, -9.0f, 0.0f, 15.0f, 20.0f, kitMacros);
}

// Global mixer: delay 3/16 ping-pong, a medium room (track levels stay at
// the defaults: only synths play).
void percMixer(Gen& g) {
    Project& p = g.app.project;
    p.setU8(pf::kMixReverb, 80);
    p.setU8(pf::kMixDelay, 75);
    p.setU8(pf::kDelayFeedback, 45);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempoPerc * 0.75f));
    p.setU8(pf::kDelayFlags, uint8_t(p.u8(pf::kDelayFlags) | 0x80));
    p.setF32(pf::kReverbSize, 0.7f);
    p.setF32(pf::kReverbDamp, 0.4f);
    g.app.markProjectDirty();
}

// A hit; vol < 0 = full (no Volume FX), else Volume vol (<= 50).
void hit(Gen& g, int t, int s, int note, int kit, int vol = -1, FxSet b = {}) {
    g.note(t, s, note, kit, vol >= 0 ? FxSet{FxId::Volume, vol > 50 ? 50 : vol} : FxSet{}, b);
}
const FxId kMacroFx[6] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC, FxId::MidiCcD, FxId::MidiCcE, FxId::MidiCcF};
// Macro FX on the lane (FX-only step; the step's instrument names the kit).
void macroAt(Gen& g, int s, int col, int macro, int v, int kit) {
    Step* st = g.step(kTLane, s);
    if (!st) return;
    st->instrument = uint8_t(kit);
    g.fx(kTLane, s, col, kMacroFx[macro], v);
}
// Ramp of macro over steps [s0, s1] from v0 to v1.
void macroRamp(Gen& g, int s0, int s1, int col, int macro, int v0, int v1, int kit) {
    for (int s = s0; s <= s1; ++s) macroAt(g, s, col, macro, v0 + (v1 - v0) * (s - s0) / (s1 - s0), kit);
}

// Straight groove bar on kit `k`: kick 1 / 3, snare 2 / 4, closed hat 8ths,
// open hat on the last 'and' when `open`.
void straightBar(Gen& g, int b0, int k, bool open, bool kick10) {
    hit(g, kTK, b0, nKick, k);
    hit(g, kTK, b0 + 8, nKick, k);
    if (kick10) hit(g, kTK, b0 + 10, nKick, k, 40);
    hit(g, kTS, b0 + 4, nSnare, k);
    hit(g, kTS, b0 + 12, nSnare, k);
    for (int s = 0; s < kBar; s += 2) {
        if (open && s == 14) hit(g, kTH, b0 + s, nHat2, k, 45);
        else hit(g, kTH, b0 + s, nHatC, k, s % 4 ? 30 : 45);
    }
}

// 1 "Kit tour" (kit A, 8 bars): one drum per bar.
void tourPattern(Gen& g) {
    g.beginPattern(1, 128, "Kit tour");
    // Bar 1: kick on the beats, a ghost on the last 'a'.
    for (int s = 0; s < kBar; s += 4) hit(g, kTK, s, nKick, kA);
    hit(g, kTK, 14, nKick, kA, 30);
    // Bar 2: snare on 2 / 4, softer on 1 / 3, a ghost at the end.
    const int b1 = kBar;
    hit(g, kTS, b1 + 0, nSnare, kA, 35);
    hit(g, kTS, b1 + 4, nSnare, kA);
    hit(g, kTS, b1 + 8, nSnare, kA, 35);
    hit(g, kTS, b1 + 12, nSnare, kA);
    hit(g, kTS, b1 + 15, nSnare, kA, 20);
    // Bar 3: closed hat 16ths, accents on the 8ths, note 65 (a little open) on
    // the last 8th.
    const int b2 = 2 * kBar;
    for (int s = 0; s < kBar; ++s) hit(g, kTH, b2 + s, s == 14 ? nHat1 : nHatC, kA, s % 2 ? 22 : (s % 4 ? 38 : 50));
    // Bar 4: open hat 67 choked by the closed hat; 66 and 65 in between.
    const int b3 = 3 * kBar;
    hit(g, kTH, b3 + 0, nHatO, kA);
    hit(g, kTH, b3 + 2, nHatC, kA, 40);   // chokes the open hat
    hit(g, kTH, b3 + 4, nHat2, kA);
    hit(g, kTH, b3 + 7, nHatC, kA, 40);
    hit(g, kTH, b3 + 8, nHatO, kA);
    hit(g, kTH, b3 + 11, nHatC, kA, 40);
    hit(g, kTH, b3 + 12, nHat1, kA);
    hit(g, kTH, b3 + 14, nHatO, kA, 45);  // rings into the cymbal bar
    // Bar 5: cymbal, a soft second hit.
    const int b4 = 4 * kBar;
    hit(g, kTCym, b4, nCym, kA);
    hit(g, kTCym, b4 + 12, nCym, kA, 35);
    // Bar 6: perc (clap) on 2 / 4, a soft one before 4.
    const int b5 = 5 * kBar;
    hit(g, kTP, b5 + 4, nClap, kA);
    hit(g, kTP, b5 + 10, nClap, kA, 35);
    hit(g, kTP, b5 + 12, nClap, kA);
    // Bar 7: toms from high to low, one per 8th (one tom channel).
    const int b6 = 6 * kBar;
    const int toms[8] = {79, 76, 74, 72, 57, 52, 47, 41};
    for (int k = 0; k < 8; ++k) hit(g, kTTom, b6 + 2 * k, toms[k], kA);
    // Bar 8: kick on 1, then the four silent notes 61 / 63 / 68 / 70 on the
    // hat track: only the kick may sound.
    const int b7 = 7 * kBar;
    hit(g, kTK, b7, nKick, kA);
    const int silent[4] = {61, 63, 68, 70};
    for (int k = 0; k < 4; ++k) hit(g, kTH, b7 + 4 + 3 * k, silent[k], kA);
    g.endPattern();
}

// 2 "Straight" (kit A).
void straightPattern(Gen& g) {
    g.beginPattern(2, 64, "Straight");
    for (int bar = 0; bar < 4; ++bar) straightBar(g, bar * kBar, kA, bar % 2 == 1, bar % 2 == 1);
    hit(g, kTCym, 0, nCym, kA, 45);
    g.endPattern();
}

// 3 "Busy" (kit B): 16th hats with accents, ghost snares, syncopated kick,
// cowbell.
void busyPattern(Gen& g) {
    g.beginPattern(3, 64, "Busy");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        const int kicks[2][4] = {{0, 7, 10, 0}, {0, 3, 10, 13}};
        for (int k = 0; k < 4; ++k)
            if (k == 0 || kicks[bar % 2][k]) hit(g, kTK, b0 + kicks[bar % 2][k], nKick, kB, k ? 45 : -1);
        hit(g, kTS, b0 + 4, nSnare, kB);
        hit(g, kTS, b0 + 12, nSnare, kB);
        const int ghosts[4] = {2, 7, 9, 15};
        for (int k = 0; k < 4; ++k) hit(g, kTS, b0 + ghosts[k], nSnare, kB, 12 + 4 * k);
        for (int s = 0; s < kBar; ++s) {
            if (s == 6 || (s == 14 && bar % 2)) hit(g, kTH, b0 + s, nHatO, kB, 40);
            else hit(g, kTH, b0 + s, nHatC, kB, s % 4 == 0 ? 50 : (s % 2 ? 18 + 3 * (s % 3) : 35));
        }
        hit(g, kTP, b0 + 3, nClap, kB, 35);
        hit(g, kTP, b0 + 11, nClap, kB, bar == 3 ? 50 : 30);
    }
    hit(g, kTCym, 0, nCym, kB, 40);
    g.endPattern();
}

// 4 "Tom fills" (kit A): a short fill closes each bar, the last bar is a
// 16th fill from high to low.
void tomPattern(Gen& g) {
    g.beginPattern(4, 64, "Tom fills");
    const int fills[3][4] = {{74, 72, 57, 52}, {84, 79, 76, 74}, {50, 47, 43, 40}};
    for (int bar = 0; bar < 3; ++bar) {
        const int b0 = bar * kBar;
        hit(g, kTK, b0, nKick, kA);
        hit(g, kTK, b0 + 8, nKick, kA);
        hit(g, kTS, b0 + 4, nSnare, kA);
        for (int s = 0; s < 12; s += 2) hit(g, kTH, b0 + s, nHatC, kA, s % 4 ? 30 : 45);
        for (int k = 0; k < 4; ++k) hit(g, kTTom, b0 + 12 + k, fills[bar][k], kA, k == 0 ? -1 : 50 - 4 * k);
    }
    const int b3 = 3 * kBar;
    hit(g, kTK, b3, nKick, kA);
    // 16 tom pitches from 88 down to 40, around the drum notes 60..71.
    const int run[16] = {88, 86, 84, 81, 79, 77, 74, 72, 57, 55, 53, 50, 48, 45, 43, 40};
    for (int s = 0; s < kBar; ++s) hit(g, kTTom, b3 + s, run[s], kA, s % 4 ? 40 : -1);
    g.endPattern();
}

// 5 "Macro sweeps" (kit A): A Tone, B Decay, C Pitch, then D Snap with E
// Drive, one bar each, back to 63 after its bar.
void macroPattern(Gen& g) {
    g.beginPattern(5, 64, "Macro sweeps");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        straightBar(g, b0, kA, true, false);
        hit(g, kTCym, b0, nCym, kA, 30);
        hit(g, kTTom, b0 + 10, bar % 2 ? 50 : 55, kA, 40);
    }
    macroRamp(g, 0, 15, 0, 0, 0, 127, kA);       // A Tone up
    macroAt(g, 16, 1, 0, kMacroCentre, kA);
    macroRamp(g, 16, 31, 0, 1, 0, 127, kA);      // B Decay short -> long
    macroAt(g, 32, 1, 1, kMacroCentre, kA);
    macroRamp(g, 32, 47, 0, 2, 127, 0, kA);      // C Pitch high -> low
    // Bar 4: D (Snap) and E (Drive) together; C back to centre on the kick.
    g.fx(kTK, 48, 1, kMacroFx[2], kMacroCentre);
    macroRamp(g, 48, 62, 0, 3, 0, 127, kA);
    macroRamp(g, 48, 62, 1, 4, 63, 127, kA);
    macroAt(g, 63, 0, 3, kMacroCentre, kA);
    macroAt(g, 63, 1, 4, kMacroCentre, kA);
    g.endPattern();
}

// 6 "Sends" (kit C): sparse groove so the echoes and the room are heard;
// the snare and the hats carry delay / reverb sends in the patch, F (Space)
// opens them further and closes them to dry.
void sendsPattern(Gen& g) {
    g.beginPattern(6, 64, "Sends");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        hit(g, kTK, b0, nKick, kC);
        hit(g, kTK, b0 + 10, nKick, kC, 45);
        hit(g, kTS, b0 + 4, nSnare, kC);
        if (bar % 2) hit(g, kTS, b0 + 12, nSnare, kC);
        else hit(g, kTS, b0 + 13, nSnare, kC, 35);
        for (int s = 2; s < kBar; s += 4) hit(g, kTH, b0 + s, s == 14 ? nHat2 : nHatC, kC, 40);
        hit(g, kTP, b0 + 8, nClap, kC, 40);   // shaker
    }
    hit(g, kTCym, 0, nCym, kC, 40);
    hit(g, kTTom, 60, 55, kC, 45);
    hit(g, kTTom, 62, 50, kC, 45);
    macroRamp(g, 0, 31, 0, 5, kMacroCentre, 127, kC);   // F Space wide open
    macroRamp(g, 32, 62, 0, 5, 127, 0, kC);              // ... down to dry
    macroAt(g, 63, 0, 5, kMacroCentre, kC);
    g.endPattern();
}

// 7 "Governor" (kit A): all six drums on one tick, every beat, then every
// 8th, then every 16th, with B (Decay) long: the kit keeps at most five
// ringing and cuts the quietest. Volume 50 and below (six drums sum).
void governorPattern(Gen& g) {
    g.beginPattern(7, 64, "Governor");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        const int every = bar == 0 ? 4 : (bar == 1 ? 4 : (bar == 2 ? 2 : 1));
        for (int s = 0; s < kBar; s += every) {
            const int v = 50 - 2 * (s % 4) - (bar == 3 ? 10 : 0);
            hit(g, kTK, b0 + s, nKick, kA, v);
            hit(g, kTS, b0 + s, nSnare, kA, v - 5);
            hit(g, kTH, b0 + s, (s / every) % 2 ? nHatC : nHatO, kA, v - 10);
            hit(g, kTCym, b0 + s, nCym, kA, v - 15);
            hit(g, kTP, b0 + s, nClap, kA, v - 10);
            hit(g, kTTom, b0 + s, 45 + (s % 5) * 3, kA, v - 5);
        }
    }
    macroAt(g, 0, 0, 1, 127, kA);              // B Decay long: more overlap
    macroAt(g, 63, 0, 1, kMacroCentre, kA);
    g.endPattern();
}

// 8 "Three kits": kit A, B, C one bar each on the same tracks, then the
// kits trade beats and close together (three slots = three voices).
void kitsPattern(Gen& g) {
    g.beginPattern(8, 64, "Three kits");
    const int kits[3] = {kA, kB, kC};
    for (int bar = 0; bar < 3; ++bar) {
        straightBar(g, bar * kBar, kits[bar], true, bar == 1);
        hit(g, kTP, bar * kBar + 12, nClap, kits[bar], 40);
    }
    const int b3 = 3 * kBar;
    for (int beat = 0; beat < 4; ++beat) {
        const int k = kits[beat % 3];
        hit(g, kTK, b3 + 4 * beat, nKick, k);
        hit(g, kTS, b3 + 4 * beat + 2, nSnare, kits[(beat + 1) % 3], 40);
        hit(g, kTH, b3 + 4 * beat + 1, nHatC, kits[(beat + 2) % 3], 30);
        hit(g, kTH, b3 + 4 * beat + 3, nHatC, k, 30);
    }
    hit(g, kTTom, b3 + 13, 52, kA, 45);
    hit(g, kTKitB, b3 + 14, 47, kB, 45);
    hit(g, kTKitC, b3 + 15, 43, kC, 45);
    // Last step: all three kits together (kick A, cymbal B, clap C).
    hit(g, kTK, b3 + 15, nKick, kA, 45);
    hit(g, kTCym, b3 + 15, nCym, kB, 35);
    hit(g, kTP, b3 + 15, nClap, kC, 40);
    g.endPattern();
}

bool percOnlySong(Gen& g) {
    g.tempo = kTempoPerc;
    if (!g.begin(false)) return false;
    setupKits(g);
    if (!g.ok) return false;
    trackNames(g, {"Kick", "Snare", "Hats", "Cymbal", "Perc", "Toms", "Macros", "Kit B"});
    trk::setName(g.app, kTKitC, "Kit C");
    tourPattern(g);
    straightPattern(g);
    busyPattern(g);
    tomPattern(g);
    macroPattern(g);
    sendsPattern(g);
    governorPattern(g);
    kitsPattern(g);
    g.playlist({1, 2, 3, 2, 4, 5, 6, 7, 8});
    percMixer(g);
    return g.ok;
}

}  // namespace perconly
