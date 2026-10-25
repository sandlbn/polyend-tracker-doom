// Songs 6-12: BLAST GRID remixes (TESTSONGS.md). Included by testsongs.cpp
// inside its anonymous namespace (it uses Gen and setupSlot from there).
//
// Source: the owner's own game soundtrack "BLAST GRID" (every song marked
// Original), extracted by bg_extract.py into bg_data.h: the parser's
// per-section channel lines, the play order, the groove and the SID / YM2151
// instruments. Converted here (exact): every note's pitch, onset and length
// (hold / rest), the order with its transposes, the tempo (0.1 BPM) and the
// groove (Swing FX), arpeggios (the chord and the game's arp rate, baked
// into one looped sample per chord, see below), the echo channel (the
// lead delayed N rows, as the game's compiler builds it), channel vol / pan
// (Volume / Panning FX), drums k / t / s / h / o / c (PERC notes, one hit per
// kit channel and row, last letter wins as in the game). Arranged (our
// choices, listed in TESTSONGS.md): which engine / track plays which channel,
// every timbre (patches and samples are our own approximations), the pan
// width, the mixer and sends.
//
// Track layout (all songs): 1 Lead (Synth 1 VAP), 2 Harm (Synth 1 VAP, the
// second VAP part), 3 Bass (Synth 2 ACD), 4 Kick / tom (Synth 3 PERC), 5
// Snare / hats / crash (PERC), 6-8 the remaining channels on sample
// instruments rendered by synth.h (sample notes play on tracks 1-8 only).
// Voice budget: VAP polyphony 2 + ACD 1 + PERC 1 (the whole kit is one
// voice) = 4 = the default Config Synth voices.

namespace bgr {

using namespace ts::bg;

enum : int { kTLead = 0, kTHarm = 1, kTBass = 2, kTKick = 3, kTSnare = 4, kTSample0 = 5 };
constexpr int kSampleTracks = 3;
constexpr int kVapPoly = 2;
// PERC notes (perc.h): kick 60, snare 62, closed hat 64, open hat 66, cymbal
// 69; any other note is the tom at that pitch (the game's tom ends on E3).
constexpr int kPercKick = 60, kPercSnare = 62, kPercHatC = 64, kPercHatO = 66, kPercCrash = 69, kPercTom = 52;
constexpr double kTickHz = 8e6 / (800.0 * 525.0 * 8.0 / 25.0);   // 59.52 Hz (mk_music TICK_HZ)

struct Variant {
    SongId id;
    const Song* src;
    bool fm;            // X16 YM2151 instruments instead of the C64 SID ones
    const char* tag;    // sample folder / patch name prefix
};
const Variant kVariants[] = {
    {SongId::BgTitle, &kTitle, false, "BG Title"},
    {SongId::BgWorld1, &kWorld1, false, "BG World 1"},
    {SongId::BgWorld4, &kWorld4, false, "BG World 4"},
    {SongId::BgBattle, &kBattle, false, "BG Battle"},
    {SongId::BgBattleFm, &kBattle, true, "BG Battle FM"},
    {SongId::BgBoss, &kBoss, false, "BG Boss"},
    {SongId::BgFinalBoss, &kBossFinal, false, "BG Final Boss"},
};
const Variant* variant(SongId id) {
    for (const Variant& v : kVariants)
        if (v.id == id) return &v;
    return nullptr;
}

// Where a source channel plays.
struct Assign {
    int track = -1;
    int instr = -1;      // sample slot 0.., or kSynthBase + slot
    bool synth = false;
};
struct Layout {
    Assign ch[8];
    int samples = 0;     // sample channels (tracks 6..)
};
Layout layout(const Song& s) {
    Layout L;
    bool harm = false;
    for (int c = 0; c < s.nchans; ++c) {
        const char* n = s.chans[c].name;
        Assign& a = L.ch[c];
        if (!std::strcmp(n, "lead")) a = {kTLead, kSynthBase + 0, true};
        else if (!std::strcmp(n, "bass")) a = {kTBass, kSynthBase + 1, true};
        else if (!harm && (!std::strcmp(n, "harm") || !std::strcmp(n, "flute"))) {
            a = {kTHarm, kSynthBase + 0, true};
            harm = true;
        } else {
            a = {kTSample0 + L.samples, L.samples, false};
            ++L.samples;
        }
    }
    return L;
}

// Tempo: 4 rows (one beat) last sum(groove) ticks of 59.52 Hz; the Tracker
// edits tempo in 0.1 BPM.
int beatTicks(const Song& s) {
    int t = 0;
    for (int r = 0; r < 4; ++r) t += s.groove[r % s.ngroove];
    return t;
}
float bpm(const Song& s) {
    const double b = 60.0 * kTickHz / beatTicks(s);
    return float(std::lround(b * 10.0)) / 10.0f;
}
// Swing FX (spec 03 §1.3): even steps last (S + 50) %, odd ones (150 - S) %
// of a straight step, so a long:short groove g0:g1 is S = 200 g0 / (g0 + g1) - 50.
int swingFx(const Song& s) {
    if (s.ngroove != 2 || s.groove[0] == s.groove[1]) return 0;
    const int v = int(std::lround(200.0 * s.groove[0] / (s.groove[0] + s.groove[1]) - 50.0));
    return v < 25 ? 25 : (v > 75 ? 75 : v);
}
// Arpeggios (`C4:047`, offsets in base 36). The game's drivers step the
// pitch through the chord every `arp` ticks while the note's gate and
// envelope carry on (mk_music: a KC write per arp step, no key-on). The
// Tracker's Arp FX re-triggers the sample on every arp note instead (spec
// 03 §3.4: note-off, then note-on; the voice restarts with a 1 ms fade,
// spec 04 §4.7), which turns a SID pluck at 30-60 arp notes per second into
// a machine gun. So each (chord, root) the song plays gets its own sample
// with the arpeggio rendered at the game's exact tick rate (synth.h Arp),
// and the step holds one plain note: the root, on an instrument tuned
// 60 - root so that it plays the sample at its own pitch and rate.
struct ArpKey {
    int chan, pitch;
    std::string offs;
};
syn::Arp arpOf(const std::string& offs, int pitch, int ticks) {
    syn::Arp a;
    a.hz = syn::kC4 * std::pow(2.0, (pitch - 60) / 12.0);
    for (char ch : offs) {
        if (a.n >= 3) break;
        a.ofs[a.n++] = ch >= 'a' ? ch - 'a' + 10 : ch - '0';
    }
    a.ticks = ticks < 1 ? 1 : ticks;
    return a;
}

// Channel vol (YM TL +0.75 dB per step) as a Volume FX: samples 50 = 0 dB,
// 0.48 dB per step (spec 04 §4.2); synths the velocity, linear (Macro 7).
int volSample(int vol) { return int(std::lround(50.0 - 0.75 * vol / 0.48)); }
int volSynth(int vol) { return int(std::lround(100.0 * std::pow(10.0, -0.75 * vol / 20.0))); }
// Hard L / R of the YM, softened to 30 % from the side.
int panFx(int pan) { return pan < 0 ? 20 : (pan > 0 ? 80 : 50); }

// "C#5" -> MIDI 73 (mk_music note_midi: C4 = 60). -1 if not a note.
int noteMidi(const char* t, int len) {
    static const int semi[7] = {9, 11, 0, 2, 4, 5, 7};   // A..G
    if (len < 2 || t[0] < 'A' || t[0] > 'G') return -1;
    int n = semi[t[0] - 'A'], i = 1;
    if (t[i] == '#') ++n, ++i;
    else if (t[i] == 'b') --n, ++i;
    const int oct = std::atoi(std::string(t + i, size_t(len - i)).c_str());
    return 12 * (oct + 1) + n;
}

std::vector<std::string> split(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (!*p || *p == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else {
            cur += *p;
        }
    }
    return out;
}

// The rows of channel c in section sec: its own line, else its echo source
// delayed echoRows rows (mk_music echo_tokens: restarts every section), else
// empty (the channel is silent in the section).
std::vector<std::string> channelRows(const Song& s, const Section& sec, int c) {
    const Chan& ch = s.chans[c];
    for (int k = 0; k < sec.nlines; ++k)
        if (sec.lines[k].chan == c) return split(sec.lines[k].toks);
    if (ch.echo >= 0)
        for (int k = 0; k < sec.nlines; ++k)
            if (sec.lines[k].chan == ch.echo) {
                const std::vector<std::string> src = split(sec.lines[k].toks);
                std::vector<std::string> out;
                out.push_back(".");
                for (int r = 1; r < ch.echoRows; ++r) out.push_back("-");
                for (int r = 0; r + ch.echoRows < sec.rows; ++r) out.push_back(src[size_t(r)]);
                return out;
            }
    return {};
}

// ---- patches ---------------------------------------------------------------

const SidInst* sidInst(const char* name) {
    for (const SidInst& i : kSid)
        if (!std::strcmp(i.name, name)) return &i;
    return nullptr;
}
const YmPatch* ymPatch(const char* name) {
    for (const YmPatch& p : kYm)
        if (!std::strcmp(p.name, name)) return &p;
    return nullptr;
}

// Synth 1 VAP from the lead's C64 SID instrument: one oscillator with the
// SID waveform (triangle 0, saw 0.5, pulse 1) and pulse width, PWM from
// LFO 1 when the instrument sweeps its width, vibrato from LFO 2 on the
// pitch, the SID attack / decay / sustain / release on the Amp Env, the
// filter open (the SID songs leave the filter off).
std::vector<ParamEdit> vapFromSid(const SidInst& s) {
    using namespace audio::vap;
    const float shape = s.wave[0] == 't' ? 0.0f : (s.wave[0] == 's' ? 0.5f : 1.0f);
    std::vector<ParamEdit> e = {
        {kShape1, shape}, {kShape2, shape}, {kOscMix, -1.0f}, {kPw1, float(s.pw) / 4096.0f - 0.5f},
        {kFilterType, 4.0f}, {kCutoff, 16000.0f}, {kResonance, 0.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.0f},
        {kAmpA, float(syn::sidAttackMs(s.ad >> 4) / 1000.0)}, {kAmpD, float(syn::sidDecayMs(s.ad & 15) / 1000.0)},
        {kAmpS, float(s.sr >> 4) / 15.0f}, {kAmpR, float(syn::sidDecayMs(s.sr & 15) / 1000.0)},
    };
    if (s.pwspd) {
        // The SID sweeps the width by |pwspd| per tick (x1.5 average) and
        // bounces inside $100..$EFF: one cycle = 2 x $E00 / speed ticks.
        const float hz = float(std::abs(s.pwspd) * 1.5 * kTickHz / (2.0 * 0xE00));
        e.push_back({kLfo1Ratio, hz});
        e.push_back({kLfo1Wave, 0.0f});   // triangle
        e.push_back({kLfo1Sync, 0.0f});
        e.push_back({kModBase + 0, 3.0f});   // LFO 1
        e.push_back({kModBase + 1, 0.0f});   // -> PW 1
        e.push_back({kModBase + 2, 0.5f});   // +-0.25 width
    }
    if (s.vibDepth) {
        // +-f / 2^(10 - depth) at rate / 256 cycles per tick.
        const float cents = float(1731.0 / double(1 << (10 - s.vibDepth)));
        e.push_back({kLfo2Ratio, float(s.vibRate * kTickHz / 256.0)});
        e.push_back({kLfo2Wave, 1.0f});   // sine
        e.push_back({kLfo2Sync, 0.0f});
        e.push_back({kModBase + 3, 4.0f});   // LFO 2
        e.push_back({kModBase + 4, 8.0f});   // -> Pitch 1
        e.push_back({kModBase + 5, cents / 1200.0f});
    }
    return e;
}

// Synth 2 ACD from the bass SID instrument: saw (or pulse for "growl"),
// a light low-pass with a short filter envelope (the pluck of the SID's fast
// decay), the SID envelope, legato glide (Glide FX on legato notes).
std::vector<ParamEdit> acdFromSid(const SidInst& s) {
    using namespace audio::acd;
    const bool pulse = s.wave[0] == 'p';
    return {
        {kSawMix, pulse ? 0.0f : 1.0f}, {kSquareMix, pulse ? 1.0f : 0.0f}, {kSubMix, 0.0f},
        {kPw, float(s.pw) / 4096.0f}, {kPwLfo, pulse ? 0.25f : 0.0f}, {kLfoHz, 0.4f},
        {kFilterType, 0.0f}, {kCutoff, 2400.0f}, {kResonance, 0.2f}, {kFilterEnv, 0.35f}, {kNoteTrack, 1.0f},
        {kModA, 0.0f}, {kModD, 0.18f}, {kModS, 0.0f}, {kModR, 0.1f},
        {kAmpA, float(syn::sidAttackMs(s.ad >> 4) / 1000.0)}, {kAmpD, float(syn::sidDecayMs(s.ad & 15) / 1000.0)},
        {kAmpS, float(s.sr >> 4) / 15.0f}, {kAmpR, float(syn::sidDecayMs(s.sr & 15) / 1000.0)},
        {kGlideMode, 2.0f}, {kGlideTime, 0.06f},
    };
}

// Synth 1 VAP for the FM variant, after the YM "brass" patch (M1 with
// feedback into C1, an octave-up pair at -10 dB, carriers with a soft
// attack): VAP has no FM, so two saws an octave apart through a low-pass
// whose envelope opens on the attack and settles (the modulator decay), and
// the YM vibrato (PMS 4) from LFO 2.
std::vector<ParamEdit> vapFmBrass() {
    using namespace audio::vap;
    return {
        {kShape1, 0.5f}, {kShape2, 0.5f}, {kTune2, 12.0f}, {kOscMix, -0.45f}, {kDetune, 6.0f},
        {kFilterType, 3.0f}, {kCutoff, 1400.0f}, {kResonance, 0.25f}, {kFilterEnvAmt, 0.32f}, {kNoteTrack, 1.0f},
        {kEnv1A, 0.03f}, {kEnv1D, 0.5f}, {kEnv1S, 0.45f}, {kEnv1R, 0.3f},
        {kAmpA, 0.02f}, {kAmpD, 0.6f}, {kAmpS, 0.8f}, {kAmpR, 0.25f},
        {kLfo2Ratio, 5.2f}, {kLfo2Wave, 1.0f}, {kLfo2Sync, 0.0f},
        {kModBase + 3, 4.0f}, {kModBase + 4, 8.0f}, {kModBase + 5, 0.006f},
    };
}
// Synth 2 ACD after the YM "bass" patch: two 2-op pairs whose modulators
// decay fast (bright attack, round sustain) = saw + square into a low-pass
// with a quick filter envelope.
std::vector<ParamEdit> acdFmBass() {
    using namespace audio::acd;
    return {
        {kSawMix, 0.7f}, {kSquareMix, 0.5f}, {kSubMix, 0.2f}, {kFilterType, 1.0f}, {kCutoff, 650.0f},
        {kResonance, 0.3f}, {kFilterEnv, 0.5f}, {kNoteTrack, 1.0f}, {kModA, 0.0f}, {kModD, 0.15f}, {kModS, 0.2f},
        {kModR, 0.1f}, {kAmpA, 0.0f}, {kAmpD, 0.5f}, {kAmpS, 0.6f}, {kAmpR, 0.12f}, {kGlideMode, 2.0f},
        {kGlideTime, 0.06f},
    };
}

// Synth 3 PERC: the engine's Init kit shaped like the game's drums. C64: a
// square-bodied kick (the SID noise burst into a falling pulse), a snappy
// noise snare (mix towards the noise), short hats. FM: an FM-bodied kick
// with the YM's E3 -> A1 sweep feel and a toned snare.
std::vector<ParamEdit> percKit(bool fm) {
    using namespace audio::perc;
    if (fm)
        return {{kKickWave, 4 * 11 + 5.0f}, {kKickPitch, 30.0f}, {kKickDecay, 0.3f}, {kKickModAmount, 0.5f},
                {kSnareMix, -0.1f}, {kSnareDecay, 0.2f}, {kHatClosed, 0.12f}, {kHatOpen, 0.5f}};
    return {{kKickWave, 2 * 11 + 6.0f}, {kKickPitch, 28.0f}, {kKickDecay, 0.18f}, {kSnareMix, 0.5f},
            {kSnareDecay, 0.16f}, {kHatClosed, 0.06f}, {kHatOpen, 0.35f}};
}

// ---- sample instruments -------------------------------------------------------

// One instrument per sample channel, rendered from the channel's source
// instrument: the SID voice (C64 versions) or the YM2151 patch (FM),
// forward loop over the sustain, release from the source envelope.
bool sampleFor(Gen& g, int slot, const char* channel, const char* inst, bool fm, const syn::Arp* arp = nullptr,
               const char* arpName = nullptr) {
    using namespace syn;
    int ls = 0;
    double relMs = 120;
    Buf b;
    if (fm) {
        const YmPatch* p = ymPatch(inst);
        if (!p) {
            g.fail("unknown YM patch");
            return false;
        }
        YmOperator op[4];
        for (int k = 0; k < 4; ++k)
            op[k] = {p->op[k].ar, p->op[k].d1r, p->op[k].d2r, p->op[k].rr, p->op[k].d1l, p->op[k].tl,
                     p->op[k].ks, p->op[k].mul, p->op[k].dt1};
        b = ymTone(p->con, p->fb, op, 1.6, ls, &relMs, arp);
    } else {
        // The SID arrangement's quiet "echo" voice plays the echo channel.
        const SidInst* s = sidInst(!std::strcmp(channel, "echo") ? "echo" : inst);
        if (!s) {
            g.fail("unknown SID instrument");
            return false;
        }
        const SidVoice v = {s->wave, s->pw, s->ad, s->sr, s->pwspd, s->vibDelay, s->vibRate, s->vibDepth};
        b = sidTone(v, 1.6, ls, arp);
        declick(b, 16, 0);
        relMs = sidDecayMs(s->sr & 15);
    }
    char name[40];
    if (arpName) std::snprintf(name, sizeof(name), "%s %s %s", fm ? "FM" : "SID", channel, arpName);
    else std::snprintf(name, sizeof(name), "%s %s", fm ? "FM" : "SID", channel);
    // Same loudness for every part (sustain RMS about -18 dBFS; a full
    // square would otherwise be 9 dB above a narrow pulse), peak <= 0.9.
    double mx = 1e-9, ss = 0;
    for (float x : b) mx = std::max(mx, double(std::fabs(x)));
    for (size_t i = size_t(ls); i < b.size(); ++i) ss += double(b[i]) * b[i];
    const double rms = std::sqrt(ss / double(b.size() - size_t(ls))) + 1e-9;
    const double peak = std::min(0.9, 0.125 * mx / rms);
    if (!g.addMono(slot, name, toI16(b, peak))) return false;
    g.loop(slot, PlayMode::ForwardLoop, uint32_t(ls));
    g.release(slot, uint16_t(relMs < 5 ? 5 : (relMs > 3000 ? 3000 : relMs)));
    return g.ok;
}

// ---- song ----------------------------------------------------------------------

struct PatKey {
    int section, transpose;
};

bool bgSong(Gen& g, SongId id) {
    const Variant* v = variant(id);
    if (!v) {
        g.fail("unknown BG song");
        return false;
    }
    const Song& s = *v->src;
    if (!g.begin()) return false;
    const Layout L = layout(s);
    if (L.samples > kSampleTracks) {
        g.fail("too many sample channels");
        return false;
    }

    // Synth slots: patches from the source instruments (our own, from the
    // engines' Init; never a factory patch).
    const char* leadInst = "square";
    const char* bassInst = "bass";
    for (int c = 0; c < s.nchans; ++c) {
        if (L.ch[c].track == kTLead) leadInst = s.chans[c].inst;
        if (L.ch[c].track == kTBass) bassInst = s.chans[c].inst;
    }
    const SidInst* leadSid = sidInst(leadInst);
    const SidInst* bassSid = sidInst(bassInst);
    if (!leadSid || !bassSid) {
        g.fail("lead / bass instrument");
        return false;
    }
    char pn[3][40];
    std::snprintf(pn[0], sizeof pn[0], "BG %s %s", v->fm ? "FM" : "SID", leadInst);
    std::snprintf(pn[1], sizeof pn[1], "BG %s %s", v->fm ? "FM" : "SID", bassInst);
    std::snprintf(pn[2], sizeof pn[2], "BG %s Kit", v->fm ? "FM" : "SID");
    setupSlot(g, 0, SynthModel::VAP, pn[0], v->fm ? vapFmBrass() : vapFromSid(*leadSid), kVapPoly, -12.0f, 0.0f,
              12.0f, 12.0f, nullptr);
    setupSlot(g, 1, SynthModel::ACD, pn[1], v->fm ? acdFmBass() : acdFromSid(*bassSid), 1, -7.0f, 0.0f, 0.0f, 0.0f,
              nullptr);
    setupSlot(g, 2, SynthModel::PERC, pn[2], percKit(v->fm), 1, -8.0f, 0.0f, 0.0f, 8.0f, nullptr);
    if (!g.ok) return false;

    // Sample channels; arp channels get their vol / pan on the instrument
    // (their arpeggio notes play other instruments, below, set up alike).
    for (int c = 0; c < s.nchans; ++c) {
        const Assign& a = L.ch[c];
        if (a.synth) continue;
        if (!sampleFor(g, a.instr, s.chans[c].name, s.chans[c].inst, v->fm)) return false;
        Instrument& in = g.ins(a.instr);
        in.reverbSend = 20;
        if (!std::strcmp(s.chans[c].name, "arp") || !std::strcmp(s.chans[c].name, "stab")) {
            in.volume = uint8_t(volSample(s.chans[c].vol));
            in.panning = uint8_t(panFx(s.chans[c].pan));
            in.delaySend = 25;
        }
    }
    // One arpeggio instrument per (channel, root, chord) the order plays,
    // after the channel instruments: the channel's settings, tuned so that
    // the root note plays the sample at its own rate.
    std::vector<ArpKey> arps;
    for (int o = 0; o < s.norder; ++o) {
        const Section& sec = s.sections[s.order[o].section];
        for (int c = 0; c < s.nchans; ++c) {
            if (L.ch[c].synth) continue;
            for (const std::string& t : channelRows(s, sec, c)) {
                const size_t colon = t.find(':');
                if (colon == std::string::npos) continue;
                const std::string body = t[0] == '^' ? t.substr(1) : t;
                const size_t cb = body.find(':');
                const ArpKey k = {c, noteMidi(body.c_str(), int(cb)) + s.order[o].transpose, body.substr(cb + 1)};
                bool have = false;
                for (const ArpKey& x : arps) have = have || (x.chan == k.chan && x.pitch == k.pitch && x.offs == k.offs);
                if (!have) arps.push_back(k);
            }
        }
    }
    if (L.samples + int(arps.size()) > kSampleInstrumentCount || int(arps.size()) > 24) {
        g.fail("too many arpeggio instruments");
        return false;
    }
    auto arpSlot = [&](int c, int pitch, const std::string& offs) {
        for (size_t i = 0; i < arps.size(); ++i)
            if (arps[i].chan == c && arps[i].pitch == pitch && arps[i].offs == offs) return L.samples + int(i);
        return -1;
    };
    for (size_t i = 0; i < arps.size(); ++i) {
        const ArpKey& k = arps[i];
        const int slot = L.samples + int(i);
        const int tune = 60 - k.pitch;
        if (tune < -24 || tune > 24) {
            g.fail("arpeggio root out of Tune range");
            return false;
        }
        static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        char an[16];
        std::snprintf(an, sizeof an, "%s%d %s", kNames[k.pitch % 12], k.pitch / 12 - 1, k.offs.c_str());
        const syn::Arp arp = arpOf(k.offs, k.pitch, s.chans[k.chan].arp);
        if (!sampleFor(g, slot, s.chans[k.chan].name, s.chans[k.chan].inst, v->fm, &arp, an)) return false;
        const Instrument& from = g.ins(L.ch[k.chan].instr);
        Instrument& in = g.ins(slot);
        in.volume = from.volume;
        in.panning = from.panning;
        in.delaySend = from.delaySend;
        in.reverbSend = from.reverbSend;
        in.tune = int8_t(tune);
    }

    // Track names.
    for (int t = 0; t < kTrackCount; ++t) trk::setName(g.app, t, "");
    for (int c = 0; c < s.nchans; ++c) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "%s", s.chans[c].name);
        if (nm[0] >= 'a' && nm[0] <= 'z') nm[0] = char(nm[0] - 32);
        trk::setName(g.app, L.ch[c].track, nm);
    }
    trk::setName(g.app, kTKick, "Kick");
    trk::setName(g.app, kTSnare, "Snare");

    // Patterns: one per distinct (section, transpose) of the order.
    std::vector<PatKey> pats;
    std::vector<int> playlist;
    const int swing = swingFx(s);
    for (int o = 0; o < s.norder; ++o) {
        const PatKey k = {s.order[o].section, s.order[o].transpose};
        int num = 0;
        for (size_t i = 0; i < pats.size(); ++i)
            if (pats[i].section == k.section && pats[i].transpose == k.transpose) num = int(i) + 1;
        if (!num) {
            pats.push_back(k);
            num = int(pats.size());
            const Section& sec = s.sections[k.section];
            char name[24];
            if (k.transpose) std::snprintf(name, sizeof name, "%s%+d", sec.name, k.transpose);
            else std::snprintf(name, sizeof name, "%s", sec.name);
            g.beginPattern(num, sec.rows, name);
            for (int c = 0; c < s.nchans; ++c) {
                const Chan& ch = s.chans[c];
                const Assign& a = L.ch[c];
                const std::vector<std::string> rows = channelRows(s, sec, c);
                if (rows.empty()) {   // silent in this section: key off (the game does)
                    g.off(a.track, 0);
                    continue;
                }
                const bool arpCh = !a.synth && (!std::strcmp(ch.name, "arp") || !std::strcmp(ch.name, "stab"));
                bool sounding = true;   // a leading hold continues the previous pattern
                for (int r = 0; r < sec.rows; ++r) {
                    const std::string& t = rows[size_t(r)];
                    if (t == "-") continue;
                    if (t == ".") {
                        if (sounding) g.off(a.track, r);
                        sounding = false;
                        continue;
                    }
                    const bool legato = t[0] == '^';
                    const std::string body = legato ? t.substr(1) : t;
                    const size_t colon = body.find(':');
                    const int n = noteMidi(body.c_str(), int(colon == std::string::npos ? body.size() : colon));
                    if (n < 0) {
                        g.fail("bad note token");
                        return false;
                    }
                    const int pitch = n + k.transpose;
                    FxSet fa, fb;
                    int instr = a.instr;
                    if (colon != std::string::npos && !a.synth) {
                        instr = arpSlot(c, pitch, body.substr(colon + 1));
                        if (instr < 0) {
                            g.fail("arpeggio instrument");
                            return false;
                        }
                    } else if (a.synth) {
                        if (ch.vol) fa = {FxId::Volume, volSynth(ch.vol)};
                        if (legato) fb = {FxId::Glide, 30};
                    } else if (!arpCh) {
                        if (ch.vol) fa = {FxId::Volume, volSample(ch.vol)};
                        if (ch.pan) fb = {FxId::Panning, panFx(ch.pan)};
                    }
                    g.note(a.track, r, pitch, instr, fa, fb);
                    sounding = true;
                }
            }
            // Drums: kit channel 6 (k, t) on track 4, channel 7 (s, h, o, c)
            // on track 5; several letters on one channel: the last wins.
            for (int l = 0; l < sec.nlines; ++l) {
                if (sec.lines[l].chan != -1) continue;
                const std::vector<std::string> rows = split(sec.lines[l].toks);
                for (int r = 0; r < sec.rows && r < int(rows.size()); ++r) {
                    char hit[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    for (char key : rows[size_t(r)]) {
                        const int dc = drumChannel(key);
                        if (dc >= 0) hit[dc] = key;
                    }
                    if (hit[6]) g.note(kTKick, r, hit[6] == 't' ? kPercTom : kPercKick, kSynthBase + 2);
                    if (hit[7]) {
                        const int n = hit[7] == 's' ? kPercSnare
                                    : hit[7] == 'h' ? kPercHatC
                                    : hit[7] == 'o' ? kPercHatO : kPercCrash;
                        g.note(kTSnare, r, n, kSynthBase + 2, hit[7] == 'h' ? FxSet{FxId::Volume, 80} : FxSet{});
                    }
                }
            }
            // The groove: Swing FX on the first step of every pattern (it
            // holds until Stop; on every pattern so a pattern played alone swings).
            if (swing) g.fx(kTKick, 0, 1, FxId::Swing, swing);
            g.endPattern();
        }
        playlist.push_back(num);
    }
    std::memset(g.app.project.playlist, 0, sizeof(g.app.project.playlist));
    for (size_t i = 0; i < playlist.size() && i < size_t(kSongSlots); ++i) g.app.project.playlist[i] = uint8_t(playlist[i]);
    g.app.project.playlistPos = 0;

    // Mixer: the delay at the echo's 3-row (3/16) spacing, a little reverb.
    Project& p = g.app.project;
    const float tempo = bpm(s);
    // Headroom: the sample tracks (samples peak near full scale) at -4 dB;
    // with the slot gains above the mix stays under full scale before the
    // limiter (the master sum clips there, spec 07 §5.4).
    for (int t = kTSample0; t < kTSample0 + kSampleTracks; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), 76);
    p.setU8(pf::kMixReverb, 60);
    p.setU8(pf::kMixDelay, 60);
    p.setU8(pf::kDelayFeedback, 30);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / tempo * 0.75f));
    p.setF32(pf::kReverbSize, 0.6f);
    p.setF32(pf::kReverbDamp, 0.45f);
    g.app.markProjectDirty();
    g.tempo = tempo;
    return g.ok;
}

// ---- checks (host test) -------------------------------------------------------------

// Note onsets per track over the song order (the playlist), from the
// patterns the App holds.
std::vector<int> playedNotes(App& a) {
    std::vector<int> n(size_t(kTrackCount), 0);
    for (int i = 0; i < kSongSlots && a.project.playlist[i]; ++i) {
        for (int t = 0; t < kTrackCount; ++t) {
            Track tr;
            a.readStoredTrack(a.project.playlist[i], t, tr);
            for (int st = 0; st <= tr.lastStep && st < kStepCount; ++st)
                if (tr.steps[st].note >= 0) ++n[size_t(t)];
        }
    }
    return n;
}

// Compares the written note counts with the source parser's (bg_data.h, one
// pass of the order). Returns "" when they match.
[[maybe_unused]] std::string checkNoteCounts(App& a, SongId id) {
    const Variant* v = variant(id);
    if (!v) return "not a BG song";
    const Song& s = *v->src;
    const Layout L = layout(s);
    const std::vector<int> n = playedNotes(a);
    std::string err;
    char line[160];
    for (int c = 0; c < s.nchans; ++c) {
        const int got = n[size_t(L.ch[c].track)];
        if (got != s.chans[c].notes) {
            std::snprintf(line, sizeof line, "%s: track %d has %d notes, source %d\n", s.chans[c].name,
                          L.ch[c].track + 1, got, s.chans[c].notes);
            err += line;
        }
    }
    if (n[kTKick] != s.kickHits || n[kTSnare] != s.snareHits) {
        std::snprintf(line, sizeof line, "drums: %d / %d hits, source %d / %d\n", n[kTKick], n[kTSnare], s.kickHits,
                      s.snareHits);
        err += line;
    }
    return err;
}

// Peak number of synth notes held at once (a note holds from its step to
// the next note or Off of its track, across the song order), counted per
// slot and capped at the slot polyphony; PERC (the whole kit) counts 1.
[[maybe_unused]] int peakSynthVoices(App& a) {
    int peak = 0;
    int heldInstr[kTrackCount];
    for (int t = 0; t < kTrackCount; ++t) heldInstr[t] = -1;
    for (int i = 0; i < kSongSlots && a.project.playlist[i]; ++i) {
        std::vector<Track> tr(static_cast<size_t>(kTrackCount));
        for (int t = 0; t < kTrackCount; ++t) a.readStoredTrack(a.project.playlist[i], t, tr[size_t(t)]);
        const int len = tr[0].lastStep + 1;
        for (int st = 0; st < len; ++st) {
            int perSlot[3] = {0, 0, 0};
            for (int t = 0; t < kTrackCount; ++t) {
                const Step& x = tr[size_t(t)].steps[st];
                if (x.note >= 0) heldInstr[t] = x.instrument;
                else if (x.note == kNoteOff) heldInstr[t] = -1;
                if (heldInstr[t] >= kSynthBase && heldInstr[t] < kSynthBase + 3) ++perSlot[heldInstr[t] - kSynthBase];
            }
            int sum = 0;
            for (int k = 0; k < 3; ++k) {
                const int poly = int(a.synthSlot(k).polyphony);
                const int cap = a.synthSlot(k).model == uint32_t(SynthModel::PERC) ? 1 : poly;
                sum += perSlot[k] < cap ? perSlot[k] : cap;
            }
            peak = sum > peak ? sum : peak;
        }
    }
    return peak;
}

}  // namespace bgr
