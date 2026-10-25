// Song 15 "OT Test 08 - Meter Check" (TESTSONGS.md). Included by
// testsongs.cpp inside its anonymous namespace.
//
// A calibration project for the Track Mixer 2/3 level meters, made to be
// photographed on stock 1.9.2: eight steady 441 Hz sines (exactly 441
// periods in a 1 s forward loop: no click at the loop point), one per
// track, no sends, pan centre. Tracks 1-4 play the same full-scale sine
// with the fader at 0 / -6 / -12 / -24 dB; tracks 5-8 play quieter sines
// (-6 / -12 / -24 / -48 dBFS) with the fader at 0 dB. Comparing the two
// rows shows whether the meter follows the fader, and its scale.

namespace metercheck {

constexpr int kSine = 0;   // instrument slots 0..4: 0, -6, -12, -24, -48 dBFS
constexpr float kSineDb[5] = {0.0f, -6.0f, -12.0f, -24.0f, -48.0f};

// The fader value whose gain is closest to `db` (TRK[] law, 86 = 0 dB).
int faderFor(float db) {
    int best = 86;
    float err = 1e9f;
    for (int v = 1; v <= 100; ++v) {
        const float d = 20.0f * std::log10(audio::trkGain(v));
        if (std::fabs(d - db) < err) {
            err = std::fabs(d - db);
            best = v;
        }
    }
    return best;
}

bool meterSong(Gen& g) {
    g.tempo = 120.0f;
    if (!g.begin()) return false;
    for (int k = 0; k < 5; ++k) {
        const float amp = 0.98f * std::pow(10.0f, kSineDb[k] / 20.0f);
        std::vector<int16_t> s(44100);
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = int16_t(std::lround(32767.0 * amp * std::sin(2.0 * M_PI * 441.0 * double(i) / 44100.0)));
        char name[32];
        std::snprintf(name, sizeof(name), "Sine %d dBFS", int(kSineDb[k]));
        if (!g.addMono(kSine + k, name, s)) return false;
        g.loop(kSine + k, PlayMode::ForwardLoop, 0);
    }
    Project& p = g.app.project;
    const float fader[8] = {0.0f, -6.0f, -12.0f, -24.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const int instr[8] = {0, 0, 0, 0, 1, 2, 3, 4};
    for (int t = 0; t < 8; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), uint8_t(faderFor(fader[t])));
    g.app.markProjectDirty();
    trackNames(g, {"0/0", "0/-6", "0/-12", "0/-24", "-6/0", "-12/0", "-24/0", "-48/0"});
    g.beginPattern(1, 64, "Meter check");
    for (int t = 0; t < 8; ++t) g.note(t, 0, 60, kSine + instr[t]);
    g.endPattern();
    g.playlist({1, 1});   // two slots: the song position moves on (host test)
    return g.ok;
}

}  // namespace metercheck
