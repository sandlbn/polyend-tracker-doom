// Sample synthesis for the test songs: drums, saw / FM tones, pads, noise FX.
// Everything is generated from formulas (no recorded material). Tonal
// samples sound at middle C (261.63 Hz): the engine plays a sample at its
// own pitch on note 60 (audio/audio_tables.h pitchRate).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ts {
namespace syn {

constexpr int kRate = 44100;
constexpr double kPi = 3.14159265358979323846;
constexpr double kC4 = 261.6255653;

using Buf = std::vector<float>;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    double uni() { return double(next() >> 8) / double(1u << 24); }   // 0..1
    double bi() { return uni() * 2.0 - 1.0; }                         // -1..1
};

inline int frames(double seconds) { return int(seconds * kRate + 0.5); }

// One-pole low-pass, coefficient from a cutoff in Hz.
struct OnePole {
    double a = 0, z = 0;
    void cutoff(double hz) { a = 1.0 - std::exp(-2.0 * kPi * hz / kRate); }
    double operator()(double x) { return z += a * (x - z); }
};

// Chamberlin state-variable filter (low / band / high outputs).
struct Svf {
    double low = 0, band = 0, f = 0.1, q = 0.5;
    void set(double hz, double damp) {
        f = 2.0 * std::sin(kPi * std::min(hz, kRate / 6.0) / kRate);
        q = damp;
    }
    void run(double x, double& lo, double& bp, double& hi) {
        low += f * band;
        hi = x - low - q * band;
        band += f * hi;
        lo = low;
        bp = band;
    }
};

// Naive saw with a light polyBLEP to keep high notes clean.
struct Saw {
    double ph = 0, inc = 0;
    void freq(double hz) { inc = hz / kRate; }
    double next() {
        double t = ph, v = 2.0 * t - 1.0;
        if (t < inc) {
            const double x = t / inc;
            v -= x + x - x * x - 1.0;
        } else if (t > 1.0 - inc) {
            const double x = (t - 1.0) / inc;
            v -= x * x + x + x + 1.0;
        }
        ph += inc;
        if (ph >= 1.0) ph -= 1.0;
        return v;
    }
};

inline double env(double t, double attack, double decay) {
    const double a = attack > 0 ? std::min(1.0, t / attack) : 1.0;
    return a * std::exp(-t / decay);
}

// Fades the first / last frames to avoid clicks.
inline void declick(Buf& b, int in = 32, int out = 256) {
    const int n = int(b.size());
    for (int i = 0; i < in && i < n; ++i) b[i] *= float(i) / in;
    for (int i = 0; i < out && i < n; ++i) b[n - 1 - i] *= float(i) / out;
}

// Normalises to `peak` of full scale and converts to int16.
inline std::vector<int16_t> toI16(const Buf& b, double peak = 0.89) {
    float m = 1e-9f;
    for (float v : b) m = std::max(m, std::fabs(v));
    const double g = peak * 32767.0 / m;
    std::vector<int16_t> o(b.size());
    for (size_t i = 0; i < b.size(); ++i) o[i] = int16_t(std::lround(std::max(-32767.0, std::min(32767.0, b[i] * g))));
    return o;
}

// Same gain for both channels of a stereo pair.
inline void toI16Stereo(const Buf& l, const Buf& r, std::vector<int16_t>& ol, std::vector<int16_t>& or_,
                        double peak = 0.89) {
    float m = 1e-9f;
    for (float v : l) m = std::max(m, std::fabs(v));
    for (float v : r) m = std::max(m, std::fabs(v));
    const double g = peak * 32767.0 / m;
    auto cv = [g](const Buf& b, std::vector<int16_t>& o) {
        o.resize(b.size());
        for (size_t i = 0; i < b.size(); ++i)
            o[i] = int16_t(std::lround(std::max(-32767.0, std::min(32767.0, b[i] * g))));
    };
    cv(l, ol);
    cv(r, or_);
}

// Crossfade loop: blends the frames before loopEnd into the frames before
// loopStart so that playback jumping from loopEnd-1 to loopStart is smooth.
inline void crossfadeLoop(Buf& b, int loopStart, int loopEnd, int xfade) {
    xfade = std::min(xfade, loopStart);
    for (int i = 0; i < xfade; ++i) {
        const double a = double(i) / xfade;
        const int d = loopEnd - xfade + i, s = loopStart - xfade + i;
        b[d] = float(b[d] * (1.0 - a) + b[s] * a);
    }
}

// ---- drums -------------------------------------------------------------

// Sine kick: pitch sweep f1 -> f0, amplitude decay, a short click.
inline Buf kick(double f0 = 48, double f1 = 160, double sweep = 0.035, double decay = 0.28, double len = 0.45,
                double click = 0.25) {
    Buf b(frames(len));
    double ph = 0;
    Rng r(7);
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        ph += 2 * kPi * (f0 + (f1 - f0) * std::exp(-t / sweep)) / kRate;
        double v = std::sin(ph) * env(t, 0.001, decay);
        if (t < 0.004) v += click * r.bi() * (1.0 - t / 0.004);
        b[i] = float(std::tanh(v * 1.4));
    }
    declick(b, 8, 512);
    return b;
}

// Snare: two damped tones plus band-limited noise.
inline Buf snare(double tone = 185, double noiseDecay = 0.16, double len = 0.35, uint32_t seed = 11) {
    Buf b(frames(len));
    Rng r(seed);
    Svf f;
    f.set(3500, 0.7);
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        const double body = (std::sin(2 * kPi * tone * t) + 0.6 * std::sin(2 * kPi * tone * 1.6 * t)) * env(t, 0.001, 0.06);
        double lo, bp, hi;
        f.run(r.bi(), lo, bp, hi);
        b[i] = float(0.55 * body + 0.9 * (bp + 0.5 * hi) * env(t, 0.001, noiseDecay));
    }
    declick(b, 8, 512);
    return b;
}

// Hi-hat: high-passed noise with metallic square partials.
inline Buf hat(double decay = 0.045, double len = 0.12, uint32_t seed = 21) {
    Buf b(frames(len));
    Rng r(seed);
    Svf f;
    f.set(7500, 0.5);
    const double partials[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        double metal = 0;
        for (double p : partials) metal += std::sin(2 * kPi * p * 7.3 * t) > 0 ? 1.0 : -1.0;
        double lo, bp, hi;
        f.run(0.6 * r.bi() + 0.08 * metal, lo, bp, hi);
        b[i] = float(hi * env(t, 0.0005, decay));
    }
    declick(b, 4, 256);
    return b;
}

// Clap: three quick noise bursts and a tail.
inline Buf clap(uint32_t seed = 31) {
    Buf b(frames(0.4));
    Rng r(seed);
    Svf f;
    f.set(1400, 0.6);
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        double e = 0;
        for (int k = 0; k < 3; ++k) {
            const double tk = t - k * 0.011;
            if (tk >= 0) e = std::max(e, std::exp(-tk / 0.006));
        }
        e = std::max(e, 0.6 * env(t - 0.022 > 0 ? t - 0.022 : 0, 0.001, 0.12) * (t > 0.022));
        double lo, bp, hi;
        f.run(r.bi(), lo, bp, hi);
        b[i] = float(bp * e);
    }
    declick(b, 4, 512);
    return b;
}

// Tom: pitched sine with a small sweep.
inline Buf tom(double f = 110, double len = 0.5) { return kick(f, f * 1.6, 0.05, 0.22, len, 0.05); }

// ---- tonal -------------------------------------------------------------

// FM tone: carrier at `hz`, modulator ratio / index with their own decay.
inline Buf fm(double hz, double ratio, double index, double indexDecay, double attack, double decay, double len,
              double feedback = 0) {
    Buf b(frames(len));
    double pc = 0, pm = 0, last = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        const double idx = index * std::exp(-t / indexDecay);
        const double m = std::sin(pm + feedback * last);
        last = m;
        b[i] = float(std::sin(pc + idx * m) * env(t, attack, decay));
        pc += 2 * kPi * hz / kRate;
        pm += 2 * kPi * hz * ratio / kRate;
    }
    declick(b, 16, 1024);
    return b;
}

// Two detuned saws through a low-pass; loops from loopStart (seconds) to the
// end with a crossfade. Returns frames of the loop start in loopStartOut.
inline Buf sawTone(double hz, double detuneCents, double cutoff, double len, double loopStart, int& loopStartOut,
                   double sub = 0.0) {
    Buf b(frames(len));
    Saw a, c;
    a.freq(hz * std::pow(2.0, detuneCents / 1200.0));
    c.freq(hz * std::pow(2.0, -detuneCents / 1200.0));
    OnePole lp1, lp2;
    lp1.cutoff(cutoff);
    lp2.cutoff(cutoff * 1.5);
    double sp = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        double v = 0.5 * (a.next() + c.next()) + sub * std::sin(sp);
        sp += 2 * kPi * hz * 0.5 / kRate;
        v = lp2(lp1(v));
        b[i] = float(v * std::min(1.0, t / 0.003));
    }
    loopStartOut = frames(loopStart);
    crossfadeLoop(b, loopStartOut, int(b.size()), frames(0.08));
    return b;
}

// Pad: root + fifth + octave, three detuned saws each, slow low-pass swell.
// Stereo when r != nullptr (the voices are spread left / right).
inline void pad(double hz, double len, double loopStart, Buf& l, Buf* r, int& loopStartOut, uint32_t seed = 41) {
    const double ratios[3] = {1.0, 1.49831, 2.0};
    const double det[3] = {-9.0, 0.0, 8.0};
    Saw osc[9];
    double pan[9];
    Rng rng(seed);
    for (int k = 0; k < 9; ++k) {
        osc[k].freq(hz * ratios[k / 3] * std::pow(2.0, det[k % 3] / 1200.0));
        osc[k].ph = rng.uni();
        pan[k] = (k % 3 == 0) ? 0.1 : (k % 3 == 1 ? 0.5 : 0.9);
    }
    const int n = frames(len);
    l.assign(n, 0.0f);
    if (r) r->assign(n, 0.0f);
    OnePole fl[2], fl2[2];
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const double cut = 700 + 900 * (0.5 - 0.5 * std::cos(2 * kPi * t / len));
        for (int c = 0; c < 2; ++c) {
            fl[c].cutoff(cut);
            fl2[c].cutoff(cut * 1.3);
        }
        double sl = 0, sr = 0;
        for (int k = 0; k < 9; ++k) {
            const double v = osc[k].next();
            sl += v * (1.0 - pan[k]);
            sr += v * pan[k];
        }
        const double a = std::min(1.0, t / 0.25);
        if (r) {
            l[i] = float(fl2[0](fl[0](sl)) * a);
            (*r)[i] = float(fl2[1](fl[1](sr)) * a);
        } else {
            l[i] = float(fl2[0](fl[0](sl + sr)) * a);
        }
    }
    loopStartOut = frames(loopStart);
    crossfadeLoop(l, loopStartOut, n, frames(0.25));
    if (r) crossfadeLoop(*r, loopStartOut, n, frames(0.25));
}

// Noise riser: band-pass sweep up plus a rising sine, `len` seconds.
inline Buf riser(double len, uint32_t seed = 51) {
    Buf b(frames(len));
    Rng r(seed);
    Svf f;
    double ph = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate, x = t / len;
        f.set(300 * std::pow(30.0, x), 0.35);
        double lo, bp, hi;
        f.run(r.bi(), lo, bp, hi);
        ph += 2 * kPi * (200 * std::pow(8.0, x)) / kRate;
        b[i] = float((0.8 * bp + 0.25 * std::sin(ph)) * x * x);
    }
    declick(b, 64, 256);
    return b;
}

// Impact: low sine drop and a noise wash.
inline Buf impact(uint32_t seed = 61) {
    Buf b(frames(1.6));
    Rng r(seed);
    OnePole lp;
    lp.cutoff(2500);
    double ph = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate;
        ph += 2 * kPi * (40 + 90 * std::exp(-t / 0.08)) / kRate;
        b[i] = float(std::sin(ph) * env(t, 0.001, 0.5) + 0.5 * lp(r.bi()) * env(t, 0.001, 0.35));
    }
    declick(b, 8, 2048);
    return b;
}


// ---- chip voices (BLAST GRID remixes, bgsongs.inc) ----------------------

// PolyBLEP step correction at phase t for a phase increment dt.
inline double blep(double t, double dt) {
    if (t < dt) {
        const double x = t / dt;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}

// Band-limited pulse of width w (0..1) at phase ph (0..1), increment dt.
inline double pulseAt(double ph, double w, double dt) {
    double v = ph < w ? 1.0 : -1.0;
    v += blep(ph, dt);
    double t2 = ph - w;
    if (t2 < 0) t2 += 1.0;
    v -= blep(t2, dt);
    return v - (2.0 * w - 1.0);   // remove the DC of an uneven pulse
}

// SID-style ADSR times (ms): attack, and decay / release (3x, to zero).
constexpr double kSidAttackMs[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
inline double sidAttackMs(int a) { return kSidAttackMs[(a < 1 ? 1 : a) & 15]; }   // 0 plays as 1 (sid_instruments.txt)
inline double sidDecayMs(int d) { return 3.0 * kSidAttackMs[d & 15]; }

// A chip arpeggio baked into a sample (the game drivers' arp: the pitch
// steps through the chord every `ticks` 59.52 Hz ticks while the note's gate
// and envelope carry on; the Tracker's Arp FX re-triggers the sample on every
// arp note instead, spec 03 §3.4). Root at `hz`, offsets in semitones.
// The phase runs on through the pitch changes (no click per arp note, as on
// the chips); the loop spans whole arp cycles from a cycle start, so it
// joins the same arp note, crossfaded over 8 ms.
struct Arp {
    double hz = kC4;
    int ofs[3] = {0, 0, 0};
    int n = 0;        // chord notes (2 or 3)
    int ticks = 0;    // ticks per arp note
    static constexpr double kTickHz = 8e6 / (800.0 * 525.0 * 8.0 / 25.0);   // 59.52 Hz
    int cycle() const { return n * ticks; }
    // Pitch ratio of the arp note that tick `t` plays.
    double mul(int t) const { return std::pow(2.0, ofs[(t / ticks) % n] / 12.0); }
    // First frame of tick t.
    static int frameOf(int t) { return int(std::ceil(double(t) * kRate / kTickHz - 1e-9)); }
    // Loop of whole cycles: start = the first cycle start at or after
    // `fromSec`, at least `minSec` long. Returns the end frame (= length).
    int loop(double fromSec, double minSec, int& loopStart) const {
        const int c = cycle();
        const int t0 = (int(std::ceil(fromSec * kTickHz)) + c - 1) / c * c;
        const int k = std::max(1, int(std::ceil(minSec * kTickHz / c)));
        loopStart = frameOf(t0);
        return frameOf(t0 + k * c);
    }
};

// A C64 SID instrument note at middle C: waveform (tri / saw / pulse),
// pulse width 0..4095 with the per-tick PWM sweep bouncing inside
// $100..$EFF, vibrato after `vibDelay` ticks, attack and decay to the
// sustain level baked in. The sustain part loops (crossfaded) from the
// returned loop start; the release is the instrument's volume envelope.
struct SidVoice {
    const char* wave;
    int pw;
    int ad, sr;
    int pwspd;
    int vibDelay, vibRate, vibDepth;
};
// With `arp`, the note is that arpeggio (root arp->hz); the length is then
// set by the loop (whole arp cycles after the decay).
inline Buf sidTone(const SidVoice& s, double len, int& loopStartOut, const Arp* arp = nullptr) {
    constexpr double kTick = 59.52;
    const double aMs = sidAttackMs(s.ad >> 4), dMs = sidDecayMs(s.ad & 15);
    int arpLoop = 0;
    const int n = arp ? arp->loop((aMs + dMs) / 1000.0 + 0.05, 0.25, arpLoop) : frames(len);
    Buf b(n);
    const double base = arp ? arp->hz : kC4;
    double arpMul = 1.0;
    int arpTick = -1;
    const double sus = double(s.sr >> 4) / 15.0;
    const bool tri = s.wave[0] == 't', saw = s.wave[0] == 's';
    double ph = 0, pw = s.pw, dir = s.pwspd >= 0 ? 1 : -1;
    const int spd = s.pwspd < 0 ? -s.pwspd : s.pwspd;
    int tick = -1;
    double vibPh = 0, vib = 1.0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const int tk = int(t * kTick);
        while (tick < tk) {   // per-tick PWM and vibrato (sid_instruments.txt)
            ++tick;
            if (spd) {
                pw += dir * spd * ((tick & 1) ? 2 : 1);
                if (pw > 0xEFF) { pw = 0xEFF; dir = -1; }
                if (pw < 0x100) { pw = 0x100; dir = 1; }
            }
            if (s.vibDepth && tick >= s.vibDelay) {
                vibPh += s.vibRate / 256.0;
                vib = 1.0 + std::sin(2 * kPi * vibPh) / double(1 << (10 - s.vibDepth));
            }
        }
        if (arp) {   // arp notes on the exact 59.52 Hz tick grid (phase runs on)
            const int at = int(double(i) * Arp::kTickHz / kRate + 1e-9);
            if (at != arpTick) arpMul = arp->mul(at);
            arpTick = at;
        }
        const double dt = base * arpMul * vib / kRate;
        double v;
        if (tri) v = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
        else if (saw) v = 2 * ph - 1 - blep(ph, dt);
        else v = pulseAt(ph, pw / 4096.0, dt);
        ph += dt;
        if (ph >= 1.0) ph -= 1.0;
        const double ms = t * 1000.0;
        double e;
        if (ms < aMs) e = ms / aMs;
        else e = sus + (1.0 - sus) * std::exp(-(ms - aMs) / (dMs / 4.6));   // ~1 % after the SID decay time
        b[i] = float(v * e);
    }
    if (arp) {
        loopStartOut = arpLoop;
        crossfadeLoop(b, loopStartOut, n, frames(0.008));
        return b;
    }
    loopStartOut = std::min(n - frames(0.3), frames((aMs + dMs) / 1000.0) + frames(0.05));
    if (loopStartOut < frames(0.05)) loopStartOut = frames(0.05);
    crossfadeLoop(b, loopStartOut, n, std::min(frames(0.12), loopStartOut));
    return b;
}

// A YM2151 patch note at middle C (instruments.txt, operators in register
// order M1 M2 C1 C2: AR D1R D2R RR D1L TL KS MUL DT1). Algorithms 4 (M1 ->
// C1, M2 -> C2, two pairs) and 7 (four carriers); others play as 4. Our own
// rough operator model: exponential (dB-linear) envelopes with the rate
// doubling every 2 steps, TL 0.75 dB, modulation index 4 pi x level,
// feedback pi x 2^(FB - 6) on M1. The tail loops from the returned start.
struct YmOperator {
    int ar, d1r, d2r, rr, d1l, tl, ks, mul, dt1;
};
inline double ymRateMs96(int r) { return 6.7 * std::pow(2.0, (31 - r) / 2.0); }   // ms for 96 dB
// With `arp`: that arpeggio (root arp->hz), looped over whole arp cycles.
inline Buf ymTone(int con, int fb, const YmOperator op[4], double len, int& loopStartOut, double* releaseMs = nullptr,
                  const Arp* arp = nullptr) {
    int arpLoop = 0;
    const int n = arp ? arp->loop(len - 0.4, 0.25, arpLoop) : frames(len);
    Buf b(n);
    double ph[4] = {0, 0, 0, 0}, fbPrev[2] = {0, 0};
    const double det[8] = {0, 1.5, 3, 4.5, 0, -1.5, -3, -4.5};   // cents per DT1
    double inc0[4], inc[4];
    for (int k = 0; k < 4; ++k) {
        inc0[k] = (arp ? arp->hz : kC4) * (op[k].mul ? op[k].mul : 0.5) * std::pow(2.0, det[op[k].dt1 & 7] / 1200.0) / kRate;
        inc[k] = inc0[k];
    }
    int arpTick = -1;
    auto level = [&](const YmOperator& o, double ms) {   // envelope in dB below TL
        const double aMs = o.ar >= 31 ? 0.0 : 0.5 * std::pow(2.0, (31 - o.ar) / 2.0);
        if (ms < aMs) return 96.0 * (1.0 - ms / aMs);
        const double t = ms - aMs, d1 = 3.0 * o.d1l;   // D1L in 3 dB steps
        const double r1 = o.d1r ? 96.0 / ymRateMs96(o.d1r) : 0.0;
        if (r1 > 0 && t * r1 < d1) return t * r1;
        const double t1 = r1 > 0 ? d1 / r1 : 1e30;
        if (t < t1) return 0.0;
        return d1 + (o.d2r ? (t - t1) * 96.0 / ymRateMs96(o.d2r) : 0.0);
    };
    const bool all = con == 7;
    for (int i = 0; i < n; ++i) {
        const double ms = 1000.0 * i / kRate;
        if (arp) {   // KC change on the tick grid (phases and envelopes carry on)
            const int at = int(double(i) * Arp::kTickHz / kRate + 1e-9);
            if (at != arpTick) {
                for (int k = 0; k < 4; ++k) inc[k] = inc0[k] * arp->mul(at);
                arpTick = at;
            }
        }
        double a[4];
        for (int k = 0; k < 4; ++k) {
            const double db = std::min(96.0, level(op[k], ms)) + 0.75 * op[k].tl;
            a[k] = std::pow(10.0, -db / 20.0);
        }
        // M1 (0) with feedback, M2 (1), C1 (2), C2 (3).
        const double fbAmt = fb ? kPi * std::pow(2.0, fb - 6) : 0.0;
        const double m1 = a[0] * std::sin(2 * kPi * ph[0] + fbAmt * 0.5 * (fbPrev[0] + fbPrev[1]));
        fbPrev[1] = fbPrev[0];
        fbPrev[0] = m1;
        const double m2 = a[1] * std::sin(2 * kPi * ph[1]);
        double out;
        if (all) out = m1 + m2 + a[2] * std::sin(2 * kPi * ph[2]) + a[3] * std::sin(2 * kPi * ph[3]);
        else out = a[2] * std::sin(2 * kPi * ph[2] + 4 * kPi * m1) + a[3] * std::sin(2 * kPi * ph[3] + 4 * kPi * m2);
        b[i] = float(out);
        for (int k = 0; k < 4; ++k) {
            ph[k] += inc[k];
            if (ph[k] >= 1.0) ph[k] -= 1.0;
        }
    }
    if (arp) {
        loopStartOut = arpLoop;
        crossfadeLoop(b, loopStartOut, n, frames(0.008));
    } else {
        loopStartOut = n - frames(0.4);
        crossfadeLoop(b, loopStartOut, n, frames(0.12));
    }
    if (releaseMs) {   // the carriers' release to -40 dB
        const YmOperator& c = op[3];
        *releaseMs = ymRateMs96(2 * c.rr + 1) * 40.0 / 96.0;
    }
    declick(b, 16, 0);
    return b;
}

}  // namespace syn
}  // namespace ts
