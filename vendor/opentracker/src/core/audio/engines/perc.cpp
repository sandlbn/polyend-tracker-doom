// PERC drum machine engine [OT] -- see perc.h for the design notes.
//
// One block (render):
//   - every ringing drum renders its mono signal into `buf` (oscillators,
//     noise, filters and its inner envelopes, coefficients at block rate);
//   - mixDrum() adds buf into the target bus with level x velocity x balance
//     pan, times the drum's amplitude envelope, which is block-linear (one
//     or two segments per block: an attack that ends inside the block is
//     exact to the sample, decays are exponential from block to block and
//     linear inside it);
//   - the target bus follows the drum's Output: "Main" drums feed the
//     waveshaper and the EQ, "Post Overdrive" the EQ only, "Post EQ" none.
//     Drums after the last active FX mix straight into the slot bus (L / R),
//     the last FX writes its output into it.
// Cost levers (OPTIMIZATION_2X item 10): parabolic sine of a fixed-point
// phase (VCVT #31 on the M4), six-square banks as packed 16-bit phases
// (UADD16), no division in any per-sample loop, at most five drums ring at
// once (the manual's "up to five drum sounds at once").
#include "audio/engines/perc.h"

#include <cmath>
#include <cstring>

#include "audio/dsp.h"

namespace ot {
namespace audio {
namespace perc {

namespace {

constexpr float kFs = kSampleRate;
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kInvBlock = 1.0f / float(kBlock);
constexpr float kIncPerHz = 4294967296.0f / kSampleRate;   // uint32 phase increment per Hz
constexpr float kInc16PerHz = 65536.0f / kSampleRate;      // 16-bit phase increment per Hz
constexpr float kMaxHz = 0.45f * kSampleRate;
constexpr float kSilent = 3.0e-4f;                          // -70 dB: a drum stage is over
constexpr float kLog2_1000 = 9.9657843f;                    // decay times are to -60 dB
constexpr int kMaxRinging = 5;                              // drums sounding at once

// ---- block-rate math (no libm in the audio path) --------------------------------
// Block-rate helpers are kept out of line (flash); per-sample ones inline.
#define OT_BLOCK_RATE __attribute__((noinline))

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x, |relative error| < 1e-4 (cubic on the fraction).
OT_BLOCK_RATE float fastExp2(float x) {
    x = clampf(x, -126.0f, 60.0f);
    int32_t i = int32_t(x);
    if (x < float(i)) --i;
    const float f = x - float(i);
    const float p = 1.0f + f * (0.69583356f + f * (0.22606716f + f * 0.078024521f));
    const uint32_t bits = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &bits, 4);
    return s * p;
}
// log2(x) for x > 0, |error| < 0.01 (block-rate use only).
OT_BLOCK_RATE float fastLog2(float x) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    const float e = float(int32_t((b >> 23) & 255u) - 127);
    b = (b & 0x7FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &b, 4);
    m -= 1.0f;
    return e + m * (1.3465553f - 0.34655535f * m);
}
inline float minT(float t) { return t < 5.0e-4f ? 5.0e-4f : t; }
// Level after n samples of a decay to -60 dB in t seconds.
OT_BLOCK_RATE float decayN(float t, float n) { return fastExp2(-kLog2_1000 * n * kInvFs / minT(t)); }
inline float decayBlock(float t) { return decayN(t, float(kBlock)); }
OT_BLOCK_RATE float noteHz(float semis) { return 440.0f * fastExp2((semis - 69.0f) * (1.0f / 12.0f)); }
inline int32_t hzInc(float hz) { return int32_t(clampf(hz, 0.0f, kMaxHz) * kIncPerHz); }
inline uint32_t hzInc16(float hz) { return uint32_t(clampf(hz, 1.0f, 0.45f * kSampleRate) * kInc16PerHz); }
// One-pole low-pass coefficient (impulse-invariant): y += a * (x - y).
OT_BLOCK_RATE float onePoleA(float hz) { return 1.0f - fastExp2(-9.0647202f * clampf(hz, 1.0f, kMaxHz) * kInvFs); }
// Attack increment per sample (ms), 2 = instant.
inline float attackInc(float ms) { return ms < 0.02f ? 2.0f : 1000.0f * kInvFs / ms; }

// ---- per-sample building blocks ----------------------------------------------------

// int32 fixed point (31 fraction bits) -> float in [-1, 1).
inline float q31(uint32_t v) {
#if defined(__arm__) && defined(__ARM_FP)
    float f;
    __asm__("vmov %0, %1" : "=t"(f) : "r"(v));
    __asm__("vcvt.f32.s32 %0, %0, #31" : "+t"(f));
    return f;
#else
    return float(int32_t(v)) * (1.0f / 2147483648.0f);
#endif
}
// Parabolic sine of a uint32 phase (one cycle = 2^32). An approximation:
// third harmonic at -30 dB, which the drum bodies tolerate.
inline float sinU(uint32_t ph) {
    const float x = q31(ph);
    return x * (4.0f - 4.0f * std::fabs(x));
}
inline float sgnU(uint32_t ph) { return int32_t(ph) < 0 ? -1.0f : 1.0f; }
inline uint32_t noiseStep(uint32_t s) { return s * 1664525u + 1013904223u; }
// Phase offset in cycles (|c| < 1) -> uint32.
inline uint32_t cyc(float c) { return uint32_t(int32_t(c * 2147483648.0f)) * 2u; }
// Two 16-bit phase accumulators in one word.
inline uint32_t add16x2(uint32_t a, uint32_t b) {
#if defined(__arm__) && defined(__ARM_FEATURE_SIMD32)
    uint32_t r;
    __asm__("uadd16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
#else
    return ((a & 0x7FFF7FFFu) + (b & 0x7FFF7FFFu)) ^ ((a ^ b) & 0x80008000u);
#endif
}
// Clamp to [-1, 1): on the M4 the saturating VCVT to Q31 and back.
inline float clamp1(float x) {
#if defined(__arm__) && defined(__ARM_FP)
    __asm__("vcvt.s32.f32 %0, %0, #31" : "+t"(x));
    __asm__("vcvt.f32.s32 %0, %0, #31" : "+t"(x));
    return x;
#else
    return clampf(x, -1.0f, 1.0f);
#endif
}
// Cubic soft clip of a value clamped to +-1.
inline float softClip(float x) {
    x = clamp1(x);
    return x * (1.5f - 0.5f * x * x);
}

// Topology-preserving SVF (Zavalishin), coefficients per block.
struct SvfK {
    float a1, a2, a3, k;
    OT_BLOCK_RATE void set(float hz, float q) {
        float x = 3.14159265f * clampf(hz, 10.0f, kMaxHz) * kInvFs;
        if (x > 1.40f) x = 1.40f;
        const float x2 = x * x;
        const float g = x * (15.0f - x2) / (15.0f - 6.0f * x2);   // tan(x), Pade
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};
struct SvfS {
    float s1, s2;
};
inline void svf(const SvfK& c, float& s1, float& s2, float x, float& lp, float& bp) {
    const float v3 = x - s2;
    const float v1 = c.a1 * s1 + c.a2 * v3;
    const float v2 = s2 + c.a2 * s1 + c.a3 * v3;
    s1 = 2.0f * v1 - s1;
    s2 = 2.0f * v2 - s2;
    lp = v2;
    bp = v1;
}

// ---- Body Wave: 16 families x 11 positions ---------------------------------------

struct Osc {
    uint32_t ph;
    int32_t inc, dInc;   // per-sample increment and its per-sample change
    float m, dM;         // morph 0..1 and its per-sample change
};

const float kCrushInv[4] = {1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f};

template <int F>
inline float wave(uint32_t ph, float m) {
    if (F == 16) return sinU(ph);   // plain sine (partners)
    const float s = sinU(ph);
    if (F == 0) {   // Sat-Sin: two cubic soft-clip passes
        const float c1 = s * (1.5f - 0.5f * s * s);
        const float c2 = c1 * (1.5f - 0.5f * c1 * c1);
        return s + 1.6f * m * (c2 - s);
    } else if (F == 1) {   // Triangle
        const float x = q31(ph);
        const float t = 1.0f - std::fabs(2.0f * std::fabs(x) - 1.0f);
        return s + m * ((x < 0.0f ? -t : t) - s);
    } else if (F == 2) {   // Square
        return s + m * (sgnU(ph) - s);
    } else if (F == 3) {   // Square FM: phase pushed by a square at 2x
        return sinU(ph + cyc(m * 0.18f * sgnU(ph * 2u)));
    } else if (F >= 4 && F <= 7) {   // FM 1 / 3 / 7 / 11
        constexpr uint32_t n = F == 4 ? 1u : (F == 5 ? 3u : (F == 6 ? 7u : 11u));
        return sinU(ph + cyc(m * 0.35f * sinU(ph * n)));
    } else if (F >= 8 && F <= 11) {   // tzFM 1 / 3 / 7 / 11 (signed-square modulator + AM)
        constexpr uint32_t n = F == 8 ? 1u : (F == 9 ? 3u : (F == 10 ? 7u : 11u));
        const float md = sinU(ph * n);
        return sinU(ph + cyc(m * 0.45f * md * std::fabs(md))) * (1.0f - 0.5f * m + 0.5f * m * md);
    } else if (F == 12) {   // Glitch 1: phase quantise (128 .. 4 steps)
        const uint32_t sh = 25u + uint32_t(m * 5.99f);
        return s + m * (sinU(ph & ~((1u << sh) - 1u)) - s);
    } else if (F == 13) {   // Glitch 2: amplitude crush (16 .. 2 levels)
        const int32_t j = int32_t(m * 3.99f);
        const float L = float(16 >> j);
        return s + m * (float(int32_t(s * L)) * kCrushInv[j] - s);
    } else if (F == 14) {   // Glitch 3: wavefold
        float w = s * (1.0f + 4.0f * m) * 0.25f + 2.25f;
        w -= float(int32_t(w));
        return 1.0f - 4.0f * std::fabs(w - 0.5f);
    } else {   // Glitch 4: phase-bit XOR
        return sinU(ph ^ (uint32_t(m * 255.99f) << 24));
    }
}

// Adds g * wave into out[0..kBlock).
template <int F>
void waveBlock(float* out, Osc& o, float g, uint32_t n) {
    uint32_t ph = o.ph;
    int32_t inc = o.inc;
    float m = o.m;
    const int32_t di = o.dInc;
    const float dm = o.dM;
    for (uint32_t i = 0; i < n; ++i) {
        ph += uint32_t(inc);
        inc += di;
        if (F != 16) m += dm;
        out[i] += g * wave<F>(ph, m);
    }
    o.ph = ph;
    o.inc = inc;
    o.m = m;
}
// Adds two plain sines (Tom partners).
void sine2Block(float* out, Osc& a, float ga, Osc& b, float gb, uint32_t n) {
    uint32_t pa = a.ph, pb = b.ph;
    int32_t ia = a.inc, ib = b.inc;
    const int32_t da = a.dInc, db = b.dInc;
    for (uint32_t i = 0; i < n; ++i) {
        pa += uint32_t(ia);
        pb += uint32_t(ib);
        ia += da;
        ib += db;
        out[i] += ga * sinU(pa) + gb * sinU(pb);
    }
    a.ph = pa;
    b.ph = pb;
}
using WaveFn = void (*)(float*, Osc&, float, uint32_t);
const WaveFn kWave[16] = {waveBlock<0>,  waveBlock<1>,  waveBlock<2>,  waveBlock<3>, waveBlock<4>,  waveBlock<5>,
                          waveBlock<6>,  waveBlock<7>,  waveBlock<8>,  waveBlock<9>, waveBlock<10>, waveBlock<11>,
                          waveBlock<12>, waveBlock<13>, waveBlock<14>, waveBlock<15>};
const WaveFn kSine = waveBlock<16>;

struct WaveSel {
    WaveFn fn;
    float pos;   // 0..1
};
// A position-0 wave without modulation is a plain sine.
inline WaveSel waveSel(float idx) {
    int i = int32_t(idx + 0.5f);
    i = i < 0 ? 0 : (i > 175 ? 175 : i);
    return WaveSel{kWave[i / 11], float(i % 11) * 0.1f};
}
// Oscillator ramp for one block of n samples (kBlock, or kBlock / 2 at
// half rate): Hz and morph at the block start / end.
OT_BLOCK_RATE void oscRamp(Osc& o, float hz0, float hz1, float m0, float m1, uint32_t n = kBlock) {
    const float r = float(kBlock / n), inv = 1.0f / float(n);
    o.inc = hzInc(hz0 * r);
    o.dInc = int32_t(float(hzInc(hz1 * r) - o.inc) * inv);
    o.m = clampf(m0, 0.0f, 1.0f);
    o.dM = (clampf(m1, 0.0f, 1.0f) - o.m) * inv;
}
inline bool isSine(float m0, float m1) { return m0 <= 0.0f && m1 <= 0.0f; }
OT_BLOCK_RATE void runOsc(const WaveSel& w, float* buf, uint32_t& ph, float hz0, float hz1, float m0, float m1, float g,
                   uint32_t n) {
    Osc o{ph, 0, 0, 0.0f, 0.0f};
    oscRamp(o, hz0, hz1, m0, m1, n);
    (isSine(o.m, o.m + o.dM * float(n)) ? kSine : w.fn)(buf, o, g, n);
    ph = o.ph;
}
// Half rate: tonal bodies whose highest partial stays below 5 kHz render
// kBlock / 2 samples and are upsampled linearly (half the oscillator cost).
inline uint32_t bodyRate(float hzTop, bool sine) {
    return hzTop * (sine ? 1.0f : 8.0f) < 5000.0f ? kBlock / 2 : kBlock;
}
// buf[0, kBlock / 2) at half rate -> buf[0, kBlock), in place; prev = the
// previous block's last half-rate sample.
OT_BLOCK_RATE void upsample2(float* buf, float& prev) {
    const float last = buf[kBlock / 2 - 1];
    for (uint32_t j = kBlock / 2 - 1; j > 0; --j) {
        const float h = buf[j], hp = buf[j - 1];
        buf[2 * j + 1] = h;
        buf[2 * j] = 0.5f * (h + hp);
    }
    const float h0 = buf[0];
    buf[1] = h0;
    buf[0] = 0.5f * (h0 + prev);
    prev = last;
}

// ---- amplitude envelope segments ----------------------------------------------------

// Up to two linear segments covering one block (attack end, then decay).
struct Env {
    uint32_t n0;     // samples of the first segment (kBlock: one segment)
    float a0, d0;    // first segment: start level, per-sample step
    float a1, d1;    // second segment
};
// Attack (linear from the current level, aInc per sample) then decay to
// -60 dB in t seconds. amp / attacking advance to the block end.
OT_BLOCK_RATE Env envStep(float& amp, uint8_t& attacking, float aInc, float t) {
    Env e;
    if (attacking) {
        const float need = (1.0f - amp) / aInc;
        if (need >= float(kBlock)) {
            e = Env{kBlock, amp, aInc, 0.0f, 0.0f};
            amp += aInc * float(kBlock);
            return e;
        }
        uint32_t n = need <= 1.0f ? 1u : uint32_t(need + 0.999f);
        if (n > kBlock) n = kBlock;
        const uint32_t rest = kBlock - n;
        e.n0 = n;
        e.a0 = amp;
        e.d0 = (1.0f - amp) / float(n);
        const float end = rest ? decayN(t, float(rest)) : 1.0f;
        e.a1 = 1.0f;
        e.d1 = rest ? (end - 1.0f) / float(rest) : 0.0f;
        amp = end;
        attacking = 0;
        return e;
    }
    const float end = amp * decayBlock(t);
    e = Env{kBlock, amp, (end - amp) * kInvBlock, 0.0f, 0.0f};
    amp = end;
    return e;
}
inline Env envConst(float a) { return Env{kBlock, a, 0.0f, 0.0f, 0.0f}; }
inline Env envRamp(float a, float b) { return Env{kBlock, a, (b - a) * kInvBlock, 0.0f, 0.0f}; }

// ---- kit state (slot scratch) ---------------------------------------------------------

struct Head {   // common to every drum channel
    uint8_t on, attacking, mode, aux;
    float vel;
    float lvl;   // envelope level at the end of the last block (0..1)
};
struct Kick {
    Head h;
    uint32_t ph;
    float amp, mod, tr, trEnv, prev;
};
struct Tom {
    Head h;
    uint32_t ph[3];
    float amp, mod, nz, nzEnv, pitch, prev;
};
struct Snare {
    Head h;
    uint32_t ph[2];
    float body, snap, sweep, prev;
    SvfS f;
};
struct Metal {   // hi-hat and cymbal: six squares (packed 16-bit) or 2-op FM + noise
    Head h;
    uint32_t ph[3];
    float e1, e2;   // hat: exponential / linear envelope; cymbal: body / impact
    float z[4];     // filter states
};
struct Perc {
    Head h;
    uint32_t ph[2];
    float env, env2, decMul;
    SvfS f;
    uint16_t cnt, spacing;
    uint8_t bursts, pad[3];
};

struct Kit {
    SynthVoice* owner;
    uint32_t token;
    uint32_t seed;
    Kick kick;
    Tom tom;
    Snare snare;
    Metal hat;
    Metal cym;
    Perc perc;
    float eq[2][4];   // per channel: low LP, high LP, mid LP pair
    float wsHold[2];
    uint32_t wsCnt;
    float sendG[2];   // send-tap gains at the end of the last block (delay, reverb)
};
static_assert(sizeof(Kit) <= kSynthSlotScratchBytes, "PERC kit state must fit the slot scratch");

struct PercVoice {
    uint32_t token;
    float vLast;   // the framework's voice gain target (last note velocity)
    float master;  // kit master at the end of the last block (1 = as hit)
};
static_assert(sizeof(PercVoice) <= kSynthVoiceStateBytes, "PERC voice state too large");

float* gTapD[kSynthSlots] = {};
float* gTapR[kSynthSlots] = {};
float gTapGD[kSynthSlots] = {};   // bus gains (slot gain x Global Mixer level)
float gTapGR[kSynthSlots] = {};

inline float P(const SynthParams& p, int i) { return p.end[i]; }

// Where a drum's mono signal goes.
struct Out {
    float* l;
    float* r;
    float gl, gr;   // level x velocity x pan
};

// buf *= env (one or two segments).
void envApply(float* buf, const Env& e) {
    float a = e.a0;
    uint32_t i = 0;
    for (; i < e.n0; ++i) {
        buf[i] *= a;
        a += e.d0;
    }
    a = e.a1;
    for (; i < kBlock; ++i) {
        buf[i] *= a;
        a += e.d1;
    }
}

// out += buf * env (one or two segments) * gains.
void mixDrum(const float* buf, const Out& o, const Env& e) {
    float* L = o.l;
    float* R = o.r;
    float al = e.a0 * o.gl, ar = e.a0 * o.gr;
    float dl = e.d0 * o.gl, dr = e.d0 * o.gr;
    uint32_t i = 0;
    for (; i < e.n0; ++i) {
        const float y = buf[i];
        L[i] += y * al;
        R[i] += y * ar;
        al += dl;
        ar += dr;
    }
    if (i < kBlock) {
        al = e.a1 * o.gl;
        ar = e.a1 * o.gr;
        dl = e.d1 * o.gl;
        dr = e.d1 * o.gr;
        for (; i < kBlock; ++i) {
            const float y = buf[i];
            L[i] += y * al;
            R[i] += y * ar;
            al += dl;
            ar += dr;
        }
    }
}

// Per-sample output of a drum loop: straight into the bus with the gains
// and a linear envelope.
struct SinkMix {
    float* L;
    float* R;
    float al, ar, dl, dr;
    void put(uint32_t i, float y) {
        L[i] += y * al;
        R[i] += y * ar;
        al += dl;
        ar += dr;
    }
};
inline SinkMix sinkMix(const Out& o, float a0, float a1) {
    const float d = (a1 - a0) * kInvBlock;
    return SinkMix{o.l, o.r, a0 * o.gl, a0 * o.gr, d * o.gl, d * o.gr};
}
// A half-rate body can be upsampled inside the mix: one envelope segment.
inline bool fusable(const Env& e) { return e.n0 >= kBlock; }
// out += upsample2(h[0, kBlock / 2)) * env * gains (see upsample2).
void mixHalf(const float* h, float& prev, const Out& o, const Env& e) {
    float* L = o.l;
    float* R = o.r;
    float al = e.a0 * o.gl, ar = e.a0 * o.gr;
    const float dl = e.d0 * o.gl, dr = e.d0 * o.gr;
    float pv = prev;
    for (uint32_t j = 0; j < kBlock / 2; ++j) {
        const float y1 = h[j], y0 = 0.5f * (pv + y1);
        pv = y1;
        L[2 * j] += y0 * al;
        R[2 * j] += y0 * ar;
        al += dl;
        ar += dr;
        L[2 * j + 1] += y1 * al;
        R[2 * j + 1] += y1 * ar;
        al += dl;
        ar += dr;
    }
    prev = pv;
}

// ---- drums ------------------------------------------------------------------------------
// Each render fills buf (zeroed by the caller), mixes it, sets h.lvl and
// returns whether the drum still sounds.

// Kick: Body Wave oscillator (Classic8 / Synced8: Mod -> wave position,
// Classic9: Mod -> pitch, 4 octaves per unit), attack / decay envelope,
// impulse (Timbre > 0) or noise-burst (Timbre < 0) transient through a
// one-pole LP 30 Hz .. 4 kHz (|Timbre|).
void kickOn(Kick& d, const SynthParams& p) {
    const int model = int32_t(clampf(P(p, kKickModel) + 0.5f, 0.0f, 2.0f));
    if (model != 0) d.ph = 0;   // Synced8 / Classic9 restart the oscillator
    d.h.attacking = 1;
    d.mod = 1.0f;
    const float t = P(p, kKickTrTimbre), lvl = P(p, kKickTrLevel);
    if (t >= 0.0f) {
        d.tr = lvl;   // impulse: the LP output starts at the level
        d.trEnv = 0.0f;
        d.h.aux = 0;
    } else {
        d.trEnv = lvl;
        d.h.aux = 1;
    }
    d.h.mode = uint8_t(model);
}
OT_BLOCK_RATE bool kickRender(Kick& d, const SynthParams& p, float bend0, float bend1, uint32_t& seed, float* buf, const Out& o) {
    const WaveSel w = waveSel(P(p, kKickWave));
    const float pitch = P(p, kKickPitch), amt = P(p, kKickModAmount);
    const float mod0 = d.mod, mod1 = mod0 * decayBlock(P(p, kKickModDecay));
    d.mod = mod1 < 1.0e-6f ? 0.0f : mod1;
    float hz0, hz1, m0 = w.pos, m1 = w.pos;
    if (d.h.mode == 2) {
        hz0 = noteHz(pitch + bend0 + 48.0f * amt * mod0);
        hz1 = noteHz(pitch + bend1 + 48.0f * amt * mod1);
    } else {
        hz0 = noteHz(pitch + bend0);
        hz1 = noteHz(pitch + bend1);
        m0 += amt * mod0;
        m1 += amt * mod1;
    }
    const uint32_t n = bodyRate(hz0 > hz1 ? hz0 : hz1, isSine(m0, m1));
    zeroWords(buf, n * sizeof(float));
    runOsc(w, buf, d.ph, hz0, hz1, m0, m1, 1.0f, n);
    const Env e = envStep(d.amp, d.h.attacking, attackInc(P(p, kKickAttack)), P(p, kKickDecay));
    bool trOn = std::fabs(d.tr) > 1.0e-5f || d.trEnv > 1.0e-5f;
    if (n < kBlock && !trOn && fusable(e)) {
        mixHalf(buf, d.prev, o, e);
        d.h.lvl = d.amp;
        return d.amp > kSilent || d.h.attacking;
    }
    if (n < kBlock) upsample2(buf, d.prev);
    else d.prev = buf[kBlock - 1];
    if (!trOn) {
        mixDrum(buf, o, e);
    } else {   // transient: its own short envelope, added after the body envelope
        const float a = onePoleA(30.0f * fastExp2(7.0589f * std::fabs(P(p, kKickTrTimbre))));
        float tr = d.tr;
        if (d.h.aux) {   // noise burst (~4 ms), LP, loudness-compensated
            const float comp = clampf(fastExp2(0.5f * fastLog2((2.0f - a) / a)), 1.0f, 10.0f);
            const float kb = decayN(0.004f, 1.0f);
            float e2 = d.trEnv * comp;
            uint32_t s = seed;
            envApply(buf, e);
            for (uint32_t i = 0; i < kBlock; ++i) {
                s = noiseStep(s);
                tr += a * (q31(s) * e2 - tr);
                e2 *= kb;
                buf[i] += tr;
            }
            seed = s;
            d.trEnv = e2 / comp;
        } else {   // impulse response of the LP
            const float k = 1.0f - a;
            envApply(buf, e);
            for (uint32_t i = 0; i < kBlock; ++i) {
                buf[i] += tr;
                tr *= k;
            }
        }
        d.tr = tr;
        mixDrum(buf, o, envConst(1.0f));
        trOn = std::fabs(d.tr) > 1.0e-5f || d.trEnv > 1.0e-5f;
    }
    d.h.lvl = d.amp;
    return d.amp > kSilent || d.h.attacking || trOn;
}

// Tom (pitch = the note): Classic = one Body Wave oscillator; Classic+ adds
// LP-filtered noise (25 ms); Resonant = three detuned oscillators + the
// noise. Mod envelope: Pitch Mod (3 semitones per unit), Wave Mod (position).
void tomOn(Tom& d, const SynthParams& p, float pitch) {
    d.pitch = pitch;
    d.h.attacking = 1;
    d.mod = 1.0f;
    d.nzEnv = 1.0f;
    d.h.mode = uint8_t(clampf(P(p, kTomModel) + 0.5f, 0.0f, 2.0f));
    d.ph[0] = d.ph[1] = d.ph[2] = 0;
}
OT_BLOCK_RATE bool tomRender(Tom& d, const SynthParams& p, float bend0, float bend1, uint32_t& seed, float* buf, const Out& o) {
    const WaveSel w = waveSel(P(p, kTomWave));
    const float pm = 3.0f * P(p, kTomPitchMod), wm = P(p, kTomWaveMod);
    const float mod0 = d.mod, mod1 = mod0 * decayBlock(P(p, kTomModDecay));
    d.mod = mod1 < 1.0e-6f ? 0.0f : mod1;
    const float hz0 = noteHz(d.pitch + bend0 + pm * mod0), hz1 = noteHz(d.pitch + bend1 + pm * mod1);
    const bool res = d.h.mode == 2;
    const float m0 = w.pos + wm * mod0, m1 = w.pos + wm * mod1, top = hz0 > hz1 ? hz0 : hz1;
    const uint32_t n = bodyRate(res ? top * 1.97f : top, isSine(m0, m1));
    zeroWords(buf, n * sizeof(float));
    runOsc(w, buf, d.ph[0], hz0, hz1, m0, m1, res ? 0.6f : 1.0f, n);
    if (res) {   // detuned partners: plain sines
        Osc a{d.ph[1], 0, 0, 0.0f, 0.0f}, b{d.ph[2], 0, 0, 0.0f, 0.0f};
        oscRamp(a, hz0 * 1.48f, hz1 * 1.48f, 0.0f, 0.0f, n);
        oscRamp(b, hz0 * 1.97f, hz1 * 1.97f, 0.0f, 0.0f, n);
        sine2Block(buf, a, 0.3f, b, 0.2f, n);
        d.ph[1] = a.ph;
        d.ph[2] = b.ph;
    }
    const Env e = envStep(d.amp, d.h.attacking, attackInc(P(p, kTomAttack)), P(p, kTomDecay));
    const bool noise = d.h.mode != 0 && d.nzEnv > kSilent;
    if (n < kBlock && !noise && fusable(e)) {
        mixHalf(buf, d.prev, o, e);
        d.h.lvl = d.amp;
        return d.amp > kSilent || d.h.attacking;
    }
    if (n < kBlock) upsample2(buf, d.prev);
    else d.prev = buf[kBlock - 1];
    if (noise) {   // noise transient, under the amp envelope (~1 for 25 ms)
        // Timbre 0..2 -> cutoff 100 Hz .. 10 kHz; level 0..1 (x2: the
        // transient's former 0..2 scale).
        const float a = onePoleA(100.0f * fastExp2(3.322f * P(p, kTomTrTimbre)));
        const float lv = 2.0f * P(p, kTomTrLevel), e1 = d.nzEnv * decayBlock(0.025f);
        float z = d.nz, e = d.nzEnv * lv;
        const float de = (e1 - d.nzEnv) * lv * kInvBlock;
        uint32_t s = seed;
        for (uint32_t i = 0; i < kBlock; ++i) {
            s = noiseStep(s);
            z += a * (q31(s) - z);
            buf[i] += z * e;
            e += de;
        }
        seed = s;
        d.nz = z;
        d.nzEnv = e1;
    }
    mixDrum(buf, o, e);
    d.h.lvl = d.amp;
    return d.amp > kSilent || d.h.attacking;
}

// Snare: Body Wave oscillator + a sine partner at the model's ratio (Tone =
// balance), Classic9 / 9+ / Modern+ add a pitch sweep; noise snap with
// attack / decay through the model's filter (Snap Timbre = cutoff 800 Hz ..
// 12.8 kHz); Body/Snap Mix balance; Modern+ saturates the body.
struct SnareModel {
    float ratio, sweep, q, cl, cb, ch, fMul;
    bool sat;
};
const SnareModel kSnareModels[5] = {
    {1.47f, 0.0f, 0.7f, 0.0f, 0.0f, 1.0f, 1.0f, false},   // Classic8: HP
    {1.56f, 4.0f, 0.8f, 0.0f, 1.0f, 0.3f, 1.0f, false},   // Classic9: BP + some HP
    {1.56f, 9.0f, 0.6f, 0.0f, 1.0f, 0.6f, 1.5f, false},   // Classic9+: brighter, deeper sweep
    {2.27f, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f, false},   // Modern: resonant BP
    {2.27f, 2.0f, 1.2f, 0.2f, 1.0f, 0.4f, 1.2f, true},    // Modern+: saturated body
};
void snareOn(Snare& d, const SynthParams& p) {
    d.h.mode = uint8_t(clampf(P(p, kSnareModel) + 0.5f, 0.0f, 4.0f));
    d.h.attacking = 1;
    d.body = 1.0f;
    d.sweep = 1.0f;
    d.ph[0] = d.ph[1] = 0;
}
// Snap noise through the model filter, plus the body already in buf, with
// their envelopes, over buf[from, to).
// Snap filter: models 0..2 = two one-poles (band = upper - lower, high =
// x - lower), models 3..4 = a resonant SVF.
struct SnapK {
    SvfK f;
    float aU, aL;       // one-pole coefficients (fc * 1.5, fc / 1.5)
    float cx, cu, cl;   // y = cx * x + cu * upper + cl * lower
};
template <bool kSat, bool kSvf, class S>
S snareLoop(float* buf, uint32_t from, uint32_t to, const SnapK& f, const SnareModel& M, float* st, uint32_t& seed,
            float& eb, float deb, float es, float des, S out) {
    float s1 = st[0], s2 = st[1], lp, bp;
    uint32_t s = seed;
    const float cl = M.cl, cb = M.cb, ch = M.ch, kk = f.f.k;
    for (uint32_t i = from; i < to; ++i) {
        s = noiseStep(s);
        const float x = q31(s);
        float y;
        if (kSvf) {
            svf(f.f, s1, s2, x, lp, bp);
            y = cl * lp + cb * bp + ch * (x - kk * bp - lp);
        } else {
            s1 += f.aU * (x - s1);
            s2 += f.aL * (x - s2);
            y = f.cx * x + f.cu * s1 + f.cl * s2;
        }
        eb += deb;
        es += des;
        const float body = kSat ? softClip(1.6f * buf[i]) : buf[i];
        out.put(i, body * eb + es * y);
    }
    seed = s;
    st[0] = s1;
    st[1] = s2;
    return out;
}
OT_BLOCK_RATE bool snareRender(Snare& d, const SynthParams& p, float bend0, float bend1, uint32_t& seed, float* buf, const Out& o) {
    const SnareModel& M = kSnareModels[d.h.mode];
    const float mix = P(p, kSnareMix);
    const float bodyG = mix > 0.0f ? 1.0f - mix : 1.0f, snapG = mix < 0.0f ? 1.0f + mix : 1.0f;
    // Body oscillators into buf.
    float b0 = 0.0f, b1 = 0.0f;
    const bool bodyOn = d.body > kSilent && bodyG > 0.0f;
    if (bodyOn) {
        const WaveSel w = waveSel(P(p, kSnareWave));
        const float pitch = P(p, kSnarePitch), tone = P(p, kSnareTone);
        const float kSw = decayBlock(0.02f);
        const float sw0 = d.sweep * M.sweep, sw1 = sw0 * kSw;
        d.sweep *= kSw;
        const float hz0 = noteHz(pitch + bend0 + sw0), hz1 = noteHz(pitch + bend1 + sw1);
        const float top = hz0 > hz1 ? hz0 : hz1;
        const uint32_t n = (tone >= 1.0f || bodyRate(top, isSine(w.pos, w.pos)) < kBlock) &&
                                   bodyRate(top * M.ratio, true) < kBlock
                               ? kBlock / 2
                               : kBlock;
        zeroWords(buf, n * sizeof(float));
        Osc q{d.ph[1], 0, 0, 0.0f, 0.0f};
        oscRamp(q, hz0 * M.ratio, hz1 * M.ratio, 0.0f, 0.0f, n);
        if (isSine(w.pos, w.pos)) {   // both plain sines: one loop
            Osc a{d.ph[0], 0, 0, 0.0f, 0.0f};
            oscRamp(a, hz0, hz1, 0.0f, 0.0f, n);
            sine2Block(buf, a, 1.0f - tone, q, tone, n);
            d.ph[0] = a.ph;
        } else {
            runOsc(w, buf, d.ph[0], hz0, hz1, w.pos, w.pos, 1.0f - tone, n);
            kSine(buf, q, tone, n);
        }
        d.ph[1] = q.ph;
        if (n < kBlock) upsample2(buf, d.prev);
        else d.prev = buf[kBlock - 1];
        b0 = d.body * bodyG;
        d.body *= decayBlock(P(p, kSnareDecay));
        b1 = d.body * bodyG;
    } else {
        d.body = 0.0f;
        zeroWords(buf, sizeof(float) * kBlock);
    }
    const bool snapOn = (d.h.attacking || d.snap > kSilent) && snapG > 0.0f;
    if (!snapOn) {
        if (snapG <= 0.0f) {
            d.snap = 0.0f;
            d.h.attacking = 0;
        }
        if (bodyOn) {
            if (M.sat)
                for (uint32_t i = 0; i < kBlock; ++i) buf[i] = softClip(1.6f * buf[i]);
            mixDrum(buf, o, envRamp(b0, b1));
        }
    } else {
        SnapK f;
        const float fc = 800.0f * fastExp2(4.0f * P(p, kSnareSnapTimbre)) * M.fMul;
        const bool svfModel = d.h.mode >= 3;
        if (svfModel) {
            f.f.set(fc, M.q);
        } else {
            f.f.k = 0.0f;
            f.aU = onePoleA(fc * 1.5f);
            f.aL = onePoleA(fc * (1.0f / 1.5f));
            const float bandG = 2.5f;   // the one-pole band peaks low: bring it up
            f.cx = M.ch;
            f.cu = M.cb * bandG;
            f.cl = M.cl - M.cb * bandG - M.ch;
        }
        const Env es = envStep(d.snap, d.h.attacking, attackInc(P(p, kSnareSnapAttack)), P(p, kSnareSnapDecay));
        float eb = b0 - (b1 - b0) * kInvBlock;   // pre-increment in the loop
        const float deb = (b1 - b0) * kInvBlock;
        float st[2] = {d.f.s1, d.f.s2};
        auto seg = [&](auto& out, uint32_t from, uint32_t to, float a, float da) {
            const float e0 = (a - da) * snapG, de = da * snapG;
            if (M.sat) out = snareLoop<true, true>(buf, from, to, f, M, st, seed, eb, deb, e0, de, out);
            else if (svfModel) out = snareLoop<false, true>(buf, from, to, f, M, st, seed, eb, deb, e0, de, out);
            else out = snareLoop<false, false>(buf, from, to, f, M, st, seed, eb, deb, e0, de, out);
        };
        SinkMix k = sinkMix(o, 1.0f, 1.0f);
        seg(k, 0, es.n0, es.a0, es.d0);
        if (es.n0 < kBlock) seg(k, es.n0, kBlock, es.a1, es.d1);
        d.f.s1 = st[0];
        d.f.s2 = st[1];
    }
    d.h.lvl = d.body > d.snap ? d.body : d.snap;
    return d.body > kSilent || d.snap > kSilent || d.h.attacking;
}

// Metal source of the hi-hat and the cymbal. Classic = six squares spread
// around Tone Pitch by Timbre (0 = all at Tone Pitch, 12 = the classic
// 808-like spread, 24 = twice that), packed as 16-bit phases; FM = 2-op FM
// with one of 25 ratio / index presets.
const float kMetalRatio[6] = {1.0f, 1.4827f, 1.8003f, 2.5460f, 2.6303f, 3.8967f};
struct MetalSrc {
    uint32_t inc[3];   // classic: packed pairs; FM: carrier, modulator (uint32)
    bool fm;
    float index;
};
OT_BLOCK_RATE MetalSrc metalSrc(float hz, float timbre, float model) {
    MetalSrc m;
    const int t = int32_t(clampf(timbre + 0.5f, 0.0f, 24.0f));
    m.fm = model >= 0.5f;
    if (!m.fm) {
        const float spread = float(t) * (1.0f / 12.0f);
        uint32_t i16[6];
        for (int j = 0; j < 6; ++j) i16[j] = hzInc16(hz * (1.0f + (kMetalRatio[j] - 1.0f) * spread)) & 0xFFFFu;
        for (int j = 0; j < 3; ++j) m.inc[j] = i16[2 * j] | (i16[2 * j + 1] << 16);
        m.index = 0.0f;
    } else {
        m.inc[0] = uint32_t(hzInc(hz * 2.0f));
        m.inc[1] = uint32_t(hzInc(hz * 2.0f * (1.4142f + 0.2137f * float(t))));
        m.inc[2] = 0;
        m.index = 0.30f + 0.055f * float((t * 7) % 12);
    }
    return m;
}
// Tone sample in [-1, 1]: classic = mean of the six squares.
template <bool kFm>
inline float metalTone(uint32_t& p0, uint32_t& p1, uint32_t& p2, const MetalSrc& m) {
    if (!kFm) {
        p0 = add16x2(p0, m.inc[0]);
        p1 = add16x2(p1, m.inc[1]);
        p2 = add16x2(p2, m.inc[2]);
        const uint32_t t = ((p0 >> 15) & 0x10001u) + ((p1 >> 15) & 0x10001u) + ((p2 >> 15) & 0x10001u);
        const int32_t c = int32_t((t & 0xFFFFu) + (t >> 16));   // squares in their low half
        return 1.0f - float(c) * (1.0f / 3.0f);
    }
    p0 += m.inc[0];
    p1 += m.inc[1];
    return sinU(p0 + cyc(m.index * sinU(p1)));
}

// Hi-hat: metal + noise (Tone/Noise Mix balance) -> 2-pole high-pass (two
// TPT one-poles) -> 1-pole low-pass; decay Closed .. Open by note, Decay
// Shape blends the exponential decay into a linear one.
OT_BLOCK_RATE float tptG(float hz) {
    float x = 3.14159265f * clampf(hz, 10.0f, kMaxHz) * kInvFs;
    if (x > 1.40f) x = 1.40f;
    const float x2 = x * x;
    const float g = x * (15.0f - x2) / (15.0f - 6.0f * x2);
    return g / (1.0f + g);
}
void hatOn(Metal& d, int variant) {
    d.h.mode = uint8_t(variant);
    d.e1 = 1.0f;
    d.e2 = 1.0f;
}
template <bool kFm, class S>
void hatLoop(Metal& d, const MetalSrc& m, float hpG, float lpA, float tg, float ng, uint32_t& seed, S out) {
    uint32_t p0 = d.ph[0], p1 = d.ph[1], p2 = d.ph[2], s = seed;
    float h1 = d.z[0], h2 = d.z[1], z = d.z[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        s = noiseStep(s);
        const float x = tg * metalTone<kFm>(p0, p1, p2, m) + ng * q31(s);
        // two one-pole high-passes (TPT): v = (x - s) G; lp = v + s; s = lp + v
        const float v1 = (x - h1) * hpG, l1 = v1 + h1;
        h1 = l1 + v1;
        const float y1 = x - l1;
        const float v2 = (y1 - h2) * hpG, l2 = v2 + h2;
        h2 = l2 + v2;
        z += lpA * ((y1 - l2) - z);
        out.put(i, z);
    }
    seed = s;
    d.ph[0] = p0;
    d.ph[1] = p1;
    d.ph[2] = p2;
    d.z[0] = h1;
    d.z[1] = h2;
    d.z[2] = z;
}
OT_BLOCK_RATE OT_FASTRUN_FN bool hatRender(Metal& d, const SynthParams& p, uint32_t& seed, const Out& o) {
    const float c = clampf(P(p, kHatClosed), 1.0e-3f, 10.0f), op = clampf(P(p, kHatOpen), 1.0e-3f, 10.0f);
    const float T = c * fastExp2(float(d.h.mode) * (1.0f / 3.0f) * (fastLog2(op) - fastLog2(c)));
    const MetalSrc m = metalSrc(P(p, kHatPitch), P(p, kHatTimbre), P(p, kHatModel));
    const float mix = P(p, kHatMix);
    const float tg = mix > 0.0f ? 1.0f - mix : 1.0f, ng = mix < 0.0f ? 1.0f + mix : 1.0f;
    const float sh = clampf(P(p, kHatShape), 0.0f, 1.0f);
    const float e0 = d.e1, l0 = d.e2;
    d.e1 = e0 * decayBlock(T);
    d.e2 = l0 - float(kBlock) * kInvFs / T;
    if (d.e2 < 0.0f) d.e2 = 0.0f;
    const float a0 = e0 + sh * (l0 - e0), a1 = d.e1 + sh * (d.e2 - d.e1);
    const float hpG = tptG(P(p, kHatHpf)), lpA = onePoleA(P(p, kHatLpf));
    const SinkMix k = sinkMix(o, a0, a1);
    if (m.fm) hatLoop<true>(d, m, hpG, lpA, tg, ng, seed, k);
    else hatLoop<false>(d, m, hpG, lpA, tg, ng, seed, k);
    d.h.lvl = a1;
    return a1 > kSilent;
}

// Cymbal: metal + noise; body stage = band-pass (SVF) at Body Filter with
// Body Decay, impact stage = one-pole high-pass at Impact Filter with Impact
// Decay; Impact/Body Mix balance.
void cymOn(Metal& d) {
    d.e1 = 1.0f;
    d.e2 = 1.0f;
}
template <bool kFm, class S>
void cymLoop(Metal& d, const MetalSrc& m, float bU, float bL, float hiA, float tg, float ng, float eb, float deb,
             float ei, float dei, uint32_t& seed, S out) {
    uint32_t p0 = d.ph[0], p1 = d.ph[1], p2 = d.ph[2], s = seed;
    float a1 = d.z[0], a2 = d.z[1], z = d.z[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        s = noiseStep(s);
        const float x = tg * metalTone<kFm>(p0, p1, p2, m) + ng * q31(s);
        a1 += bU * (x - a1);   // body band: two one-poles around Body Filter
        a2 += bL * (x - a2);
        z += hiA * (x - z);    // impact: one-pole high-pass
        eb += deb;
        ei += dei;
        out.put(i, eb * (a1 - a2) + ei * (x - z));
    }
    seed = s;
    d.ph[0] = p0;
    d.ph[1] = p1;
    d.ph[2] = p2;
    d.z[0] = a1;
    d.z[1] = a2;
    d.z[2] = z;
}
OT_BLOCK_RATE bool cymRender(Metal& d, const SynthParams& p, uint32_t& seed, const Out& o) {
    const MetalSrc m = metalSrc(P(p, kCymPitch), P(p, kCymTimbre), P(p, kCymModel));
    const float bf = P(p, kCymBodyF), bU = onePoleA(bf * 1.4f), bL = onePoleA(bf * (1.0f / 1.4f));
    const float hiA = onePoleA(P(p, kCymImpactF));
    const float mix = P(p, kCymMix), ib = P(p, kCymImpactBody);
    const float tg = mix > 0.0f ? 1.0f - mix : 1.0f, ng = mix < 0.0f ? 1.0f + mix : 1.0f;
    const float gi = ib > 0.0f ? 1.0f - ib : 1.0f, gb = ib < 0.0f ? 1.0f + ib : 1.0f;
    const float b0 = gb > 0.0f ? d.e1 : 0.0f, i0 = gi > 0.0f ? d.e2 : 0.0f;
    const float b1 = b0 * decayBlock(P(p, kCymBodyDecay)), i1 = i0 * decayBlock(P(p, kCymImpactDecay));
    const float bandG = 3.0f;   // the one-pole band peaks low: bring it up
    // Output scale: metal + noise sum to ~2 and the impact passes them at
    // full level, so the Init cymbal peaked +6 dBFS at slot gain 0 dB (the
    // other drums ~0 dBFS, like a full-scale sample): halve it.
    constexpr float kOut = 0.5f;
    const float eb = b0 * gb * bandG * kOut, deb = (b1 - b0) * gb * bandG * kOut * kInvBlock, ei = i0 * gi * kOut,
                dei = (i1 - i0) * gi * kOut * kInvBlock;
    const SinkMix k = sinkMix(o, 1.0f, 1.0f);
    if (m.fm) cymLoop<true>(d, m, bU, bL, hiA, tg, ng, eb, deb, ei, dei, seed, k);
    else cymLoop<false>(d, m, bU, bL, hiA, tg, ng, eb, deb, ei, dei, seed, k);
    d.e1 = b1;
    d.e2 = i1;
    d.h.lvl = b1 > i1 ? b1 : i1;
    return d.h.lvl > kSilent;
}

// Perc: Clap / Clap+ (band-passed noise bursts + tail; Variation = burst
// spacing), Maracas (high-passed noise; Variation = per-hit randomness of
// the decay), Shaker (band-passed noise with a swell; Variation = swell
// time), Cowbell (two squares through a band-pass; Variation = interval).
// Timbre = filter frequency / pitch, Decay = the tail.
void percOn(Perc& d, const SynthParams& p, uint32_t& seed) {
    const int model = int32_t(clampf(P(p, kPercModel) + 0.5f, 0.0f, 4.0f));
    const float var = P(p, kPercVariation);
    d.h.mode = uint8_t(model);
    d.env = 0.0f;
    d.env2 = 0.0f;
    d.h.attacking = 0;
    d.decMul = 1.0f;
    if (model <= 1) {
        d.bursts = uint8_t(model == 0 ? 4 : 5);
        d.spacing = uint16_t((0.006f + 0.008f * var) * kFs);
        d.cnt = 1;
    } else if (model == 2) {
        seed = noiseStep(seed);
        d.decMul = 1.0f + 0.6f * var * q31(seed);
        d.env = 1.0f;
    } else if (model == 3) {
        d.h.attacking = 1;
    } else {
        d.env = 1.0f;
        d.env2 = 1.0f;
        d.ph[0] = d.ph[1] = 0;
    }
}
OT_BLOCK_RATE bool percRender(Perc& d, const SynthParams& p, uint32_t& seed, float* buf, const Out& o) {
    const float var = P(p, kPercVariation), tim = P(p, kPercTimbre), dec = P(p, kPercDecay);
    SvfK f;
    float s1 = d.f.s1, s2 = d.f.s2, lp, bp;
    uint32_t s = seed;
    Env env = envConst(1.0f);
    bool on;
    // The loop of each model writes into a sink: the bus (envelope ramp in
    // the sink) or buf (an envelope with two segments, then mixDrum).
    auto emit = [&](auto loop) {
        const bool dir = fusable(env);
        if (dir) {
            loop(sinkMix(o, env.a0, env.a0 + env.d0 * float(kBlock)));
        } else {
            zeroWords(buf, sizeof(float) * kBlock);
            loop(SinkMix{buf, buf, 1.0f, 0.0f, 0.0f, 0.0f});
            mixDrum(buf, o, env);
        }
    };
    switch (d.h.mode) {
        case 0:
        case 1: {   // claps: per-sample bursts
            const bool plus = d.h.mode == 1;
            f.set(700.0f * fastExp2(2.2f * tim), plus ? 1.0f : 1.6f);
            const float kb = decayN(0.012f, 1.0f), kt = decayN(dec * (plus ? 1.6f : 1.0f), 1.0f);
            uint32_t cnt = d.cnt, bursts = d.bursts;
            const uint32_t spacing = d.spacing ? d.spacing : 1u;
            float e = d.env, e2 = d.env2;
            const float g = f.k * 1.6f;
            emit([&](auto out) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    if (bursts && --cnt == 0) {
                        e = 1.0f;
                        cnt = spacing;
                        if (--bursts == 0) e2 = 1.0f;
                    }
                    e *= kb;
                    e2 *= kt;
                    s = noiseStep(s);
                    svf(f, s1, s2, q31(s), lp, bp);
                    out.put(i, g * bp * (e + 0.7f * e2));
                }
            });
            d.cnt = uint16_t(cnt);
            d.bursts = uint8_t(bursts);
            d.env = e;
            d.env2 = e2;
            on = bursts || e > kSilent || e2 > kSilent;
            d.h.lvl = e > e2 ? e : e2;
            break;
        }
        case 2: {   // Maracas
            f.set(3000.0f * fastExp2(1.7f * tim), 0.8f);
            const float kk = f.k;
            const float e1 = d.env * decayBlock(dec * 0.5f * d.decMul);
            env = envRamp(d.env, e1);
            emit([&](auto out) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    s = noiseStep(s);
                    const float x = q31(s);
                    svf(f, s1, s2, x, lp, bp);
                    out.put(i, x - kk * bp - lp);
                }
            });
            d.env = e1;
            on = e1 > kSilent;
            d.h.lvl = e1;
            break;
        }
        case 3: {   // Shaker
            f.set(2500.0f * fastExp2(2.0f * tim), 1.2f);
            const float g = f.k * 1.4f;
            env = envStep(d.env, d.h.attacking, attackInc(2.0f + 60.0f * var), dec);
            emit([&](auto out) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    s = noiseStep(s);
                    svf(f, s1, s2, q31(s), lp, bp);
                    out.put(i, g * bp);
                }
            });
            on = d.h.attacking || d.env > kSilent;
            d.h.lvl = d.env;
            break;
        }
        default: {   // Cowbell
            const float f1 = 540.0f * fastExp2(2.0f * (tim - 0.5f));
            const float f2 = f1 * (1.48f + 0.3f * (var - 0.5f));
            f.set(f2 * 1.1f, 1.5f);
            const int32_t i1 = hzInc(f1), i2 = hzInc(f2);
            uint32_t p1 = d.ph[0], p2 = d.ph[1];
            const float g = f.k * 0.8f;
            const float e1 = d.env * decayBlock(0.05f), e2 = d.env2 * decayBlock(dec);
            env = envRamp(0.6f * d.env + 0.4f * d.env2, 0.6f * e1 + 0.4f * e2);
            emit([&](auto out) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    p1 += uint32_t(i1);
                    p2 += uint32_t(i2);
                    svf(f, s1, s2, sgnU(p1) + sgnU(p2), lp, bp);
                    out.put(i, g * bp);
                }
            });
            d.ph[0] = p1;
            d.ph[1] = p2;
            d.env = e1;
            d.env2 = e2;
            on = e1 > kSilent || e2 > kSilent;
            d.h.lvl = 0.6f * e1 + 0.4f * e2;
            break;
        }
    }
    seed = s;
    d.f.s1 = s1;
    d.f.s2 = s2;
    return on;
}

// ---- FX: waveshaper and EQ ------------------------------------------------------------
// Both run on the kit bus `x` in place; with `out` they add their result
// into out instead (the last FX of the chain feeds the slot bus).

template <int T, bool kOut>
void wsLoop(float* a, float* o, float level, float& h, uint32_t& n) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float v = a[i];
        float y;
        if (T == 0) {   // Saturate
            y = softClip(1.5f * v);
        } else if (T == 1) {   // Overdrive
            y = 0.85f * softClip(4.0f * v);
        } else if (T == 2) {   // Overdrive+: harder, then folded back a little
            const float c = softClip(12.0f * v);
            y = 0.8f * c * (1.25f - 0.25f * c * c);
        } else if (T == 3) {   // Bitcrush: 6 bits
            y = float(int32_t(clamp1(v) * 32.0f)) * (1.0f / 32.0f);
        } else {   // Bitcrush+: 4 bits, sample hold 1/4
            if ((n++ & 3u) == 0) h = float(int32_t(clamp1(v) * 8.0f)) * (1.0f / 8.0f);
            y = h;
        }
        y = v + level * (y - v);
        if (kOut) o[i] += y;
        else a[i] = y;
    }
}
using WsFn = void (*)(float*, float*, float, float&, uint32_t&);
const WsFn kWs[5][2] = {{wsLoop<0, false>, wsLoop<0, true>}, {wsLoop<1, false>, wsLoop<1, true>},
                        {wsLoop<2, false>, wsLoop<2, true>}, {wsLoop<3, false>, wsLoop<3, true>},
                        {wsLoop<4, false>, wsLoop<4, true>}};
OT_BLOCK_RATE void waveshaper(Kit& k, int type, float level, float* const x[2], float* const out[2]) {
    const WsFn fn = kWs[type][out ? 1 : 0];
    for (int c = 0; c < 2; ++c) {
        uint32_t n = k.wsCnt;
        fn(x[c], out ? out[c] : nullptr, level, k.wsHold[c], n);
        if (c == 1) k.wsCnt = n;
    }
}

inline float dbGain(float db) { return fastExp2(db * 0.16609640f); }   // 10^(db/20)

// 3-band EQ: low shelf 80 Hz and high shelf 12 kHz from one-pole splits, mid
// peak 2.5 kHz from the difference of two one-poles (1.6 kHz / 4 kHz).
// Bands with 0 dB are skipped (kMask: 1 low, 2 mid, 4 high).
template <bool kOut, int kMask>
void eqLoop(float* a, float* o, float* st, float gl, float gm, float gh, float aL, float aH, float aM1, float aM2) {
    float zl = st[0], zh = st[1], m1 = st[2], m2 = st[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float v = a[i];
        float y = v;
        if (kMask & 1) {
            zl += aL * (v - zl);
            y += gl * zl;
        }
        if (kMask & 2) {
            m1 += aM1 * (v - m1);
            m2 += aM2 * (v - m2);
            y += gm * (m1 - m2);
        }
        if (kMask & 4) {
            zh += aH * (v - zh);
            y += gh * (v - zh);
        }
        if (kOut) o[i] += y;
        else a[i] = y;
    }
    st[0] = zl;
    st[1] = zh;
    st[2] = m1;
    st[3] = m2;
}
OT_BLOCK_RATE void eq(Kit& k, float lo, float mid, float hi, float* const x[2], float* const out[2]) {
    const float gl = dbGain(lo) - 1.0f, gm = 1.9f * (dbGain(mid) - 1.0f), gh = dbGain(hi) - 1.0f;
    const float aL = onePoleA(80.0f), aH = onePoleA(12000.0f), aM1 = onePoleA(4000.0f), aM2 = onePoleA(1560.0f);
    using EqFn = void (*)(float*, float*, float*, float, float, float, float, float, float, float);
    static const EqFn kEq[2][8] = {
        {nullptr, eqLoop<false, 1>, eqLoop<false, 2>, eqLoop<false, 3>, eqLoop<false, 4>, eqLoop<false, 5>,
         eqLoop<false, 6>, eqLoop<false, 7>},
        {nullptr, eqLoop<true, 1>, eqLoop<true, 2>, eqLoop<true, 3>, eqLoop<true, 4>, eqLoop<true, 5>, eqLoop<true, 6>,
         eqLoop<true, 7>}};
    const int mask = (lo != 0.0f ? 1 : 0) | (mid != 0.0f ? 2 : 0) | (hi != 0.0f ? 4 : 0);
    if (!mask) return;
    for (int c = 0; c < 2; ++c) kEq[out ? 1 : 0][mask](x[c], out ? out[c] : nullptr, k.eq[c], gl, gm, gh, aL, aH, aM1, aM2);
}

// ---- engine ------------------------------------------------------------------------------

inline Kit& kitOf(SynthVoice& v) { return v.slotScratch->as<Kit>(); }
inline Head& headOf(Kit& k, int d) {
    switch (d) {
        case kKick: return k.kick.h;
        case kTom: return k.tom.h;
        case kSnare: return k.snare.h;
        case kHat: return k.hat.h;
        case kCymbal: return k.cym.h;
        default: return k.perc.h;
    }
}

void kitClear(Kit& k) {
    static_assert(sizeof(Kit) % 4 == 0 && alignof(Kit) >= 4, "word-wise clear");
    uint32_t* w = reinterpret_cast<uint32_t*>(&k);
    for (uint32_t i = 0; i < sizeof(Kit) / 4; ++i) {
        OT_NO_LIBCALL();
        w[i] = 0;
    }
}
// Clear the drums, keep the ownership token and the noise seed.
void kitSilence(Kit& k) {
    const uint32_t t = k.token, s = k.seed;
    kitClear(k);
    k.token = t;
    k.seed = s ? s : 0x2545F491u;
}

// ---- per-drum sends (send tap) --------------------------------------------------------
// Exact per-drum sends would cost two more multiply-adds per sample and drum
// (~6k per block for a five-drum kit, over the 30k budget). Instead the kit
// output (the slot bus after the drums and FX, mono (L + R) / 2) feeds the
// send buses with each send's level-weighted mean over the ringing drums:
// sum(level x velocity x mixer level x send) / sum(level x velocity x mixer
// level), from the drum envelopes at the block end, ramped across the
// block. A drum alone sends exactly its own amount; drums ringing together
// share one send amount weighted by how loud each is (an approximation).
// The send buses get the slot gain x Global Mixer level (gTapGD / gTapGR).
// The framework hands the voice a slot bus that carries only this kit (one
// PERC voice owns the kit; a handed-over voice adds nothing).
bool kitSends(const SynthParams& p) {
    for (int d = 0; d < kDrums; ++d)
        if (P(p, kMixer + 5 * d + 2) > 0.0f || P(p, kMixer + 5 * d + 3) > 0.0f) return true;
    return false;
}
void sendTap(Kit& k, const float* L, const float* R, float* tapD, float* tapR, float gD, float gR) {
    if (!tapD) gD = 0.0f;
    if (!tapR) gR = 0.0f;
    // From silence (a send of 0) the new amount applies at once: the drum that
    // brings it starts in this block.
    float d = k.sendG[0] > 0.0f ? k.sendG[0] : gD, r = k.sendG[1] > 0.0f ? k.sendG[1] : gR;
    const float dd = (gD - d) * (0.5f * kInvBlock), dr = (gR - r) * (0.5f * kInvBlock);
    d *= 0.5f;   // (L + R) / 2 folded into the gains
    r *= 0.5f;
    k.sendG[0] = gD;
    k.sendG[1] = gR;
    if (tapD && tapR) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float m = L[i] + R[i];
            d += dd;
            r += dr;
            tapD[i] += m * d;
            tapR[i] += m * r;
        }
    } else if (tapD || tapR) {
        float* T = tapD ? tapD : tapR;
        float g = tapD ? d : r;
        const float dg = tapD ? dd : dr;
        for (uint32_t i = 0; i < kBlock; ++i) {
            g += dg;
            T[i] += (L[i] + R[i]) * g;
        }
    }
}

// ---- cost governor (OPTIMIZATION_2X item 10: PERC <= 30k per slot) ----------------
// Steady M4 instruction cost per block of each ringing drum (mixing
// included) and of the FX, measured on the M4 bench (worst model of each
// row, tests/audio_bench.cpp "perc" components) and rounded up. A hit or a
// parameter change that would take the kit over kBudget (30k less 1k of
// headroom for the hit block's note-on work), or over five ringing drums,
// cuts the quietest other drums. The short transients (kick click, tom
// noise: at most 25 ms) count kCostTransient while they last, the send tap
// 1.4k whenever a drum send is up. Keep this table in step with the kernels
// (audio_bench perc.pk <= 30k).
constexpr int32_t kBudget = kKitBudget - 1000;   // headroom for the transients
constexpr int32_t kCostBase = 400;   // render overhead with any drum ringing
constexpr int32_t kCostTransient = 1500;   // kick click / tom noise while they last (<= 25 ms)
int32_t drumCost(Kit& k, int d) {
    switch (d) {
        case kKick: return 4600 + (std::fabs(k.kick.tr) > 1.0e-5f || k.kick.trEnv > 1.0e-5f ? kCostTransient : 0);
        case kTom:
            return (k.tom.h.mode == 2 ? 5900 : 4000) + (k.tom.h.mode != 0 && k.tom.nzEnv > kSilent ? kCostTransient : 0);
        case kSnare: return k.snare.h.mode == 4 ? 9000 : (k.snare.h.mode == 3 ? 8000 : 7400);
        case kHat: return 6100;
        case kCymbal: return 6200;
        default: return 4400;
    }
}
int32_t fxCost(const SynthParams& p) {
    int32_t c = 0;
    if (P(p, kWsLevel) > 0.0f) c += 4300;
    const int bands = (P(p, kEqLow) != 0.0f) + (P(p, kEqMid) != 0.0f) + (P(p, kEqHigh) != 0.0f);
    if (bands) c += 1300 + 1450 * bands;
    if (kitSends(p)) c += 1400;   // sendTap (the framework supplies a tap whenever a level is up)
    return c;
}
int32_t kitCostOf(Kit& k, const SynthParams& p, int* ringing) {
    int32_t cost = 0;
    int n = 0;
    for (int j = 0; j < kDrums; ++j)
        if (headOf(k, j).on) {
            ++n;
            cost += drumCost(k, j);
        }
    if (ringing) *ringing = n;
    return n ? cost + kCostBase + fxCost(p) : 0;
}
void enforceBudget(Kit& k, const SynthParams& p, int keep) {
    for (;;) {
        int ringing = 0;
        const int32_t cost = kitCostOf(k, p, &ringing);
        if (cost <= kBudget && ringing <= kMaxRinging) return;
        int q = -1;
        for (int j = 0; j < kDrums; ++j) {
            const Head& h = headOf(k, j);
            if (h.on && j != keep && (q < 0 || h.lvl * h.vel < headOf(k, q).lvl * headOf(k, q).vel)) q = j;
        }
        if (q < 0) return;
        headOf(k, q).on = 0;
    }
}

class PercEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEnginePerc; }
    const char* name() const override { return "PERC"; }

    void slotReset(SynthSlotScratch& slot) override { kitSilence(slot.as<Kit>()); }

    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        Kit& k = kitOf(v);
        PercVoice& pv = v.as<PercVoice>();
        if (!n.reused || pv.token != k.token || k.owner != &v) {
            // A new voice takes the kit over. Its drums keep ringing only when
            // another, still-owning record of this slot holds them (poly > 1);
            // a kit last owned by this record is left over from a voice that
            // already ended (or was stopped).
            const bool keep = k.owner && k.owner != &v && k.owner->slotScratch == v.slotScratch &&
                              k.owner->as<PercVoice>().token == k.token;
            if (!keep) kitSilence(k);
            k.owner = &v;
            pv.token = ++k.token;
            pv.master = 1.0f;
        }
        // A kit master left by a Volume FX stays with the drums it scaled.
        if (pv.master != 1.0f) {
            for (int j = 0; j < kDrums; ++j) {
                Head& hj = headOf(k, j);
                hj.vel = hj.vel * pv.master > 1.0f ? 1.0f : hj.vel * pv.master;
            }
            pv.master = 1.0f;
        }
        pv.vLast = n.velocity;
        int variant = 0;
        const Drum d = drumForNote(n.note, &variant);
        if (d == kNone) return;
        Head& h = headOf(k, d);
        h.on = 1;
        h.vel = n.velocity;
        h.lvl = 1.0f;
        switch (d) {
            case kKick: kickOn(k.kick, p); break;
            case kTom: tomOn(k.tom, p, n.pitch); break;
            case kSnare: snareOn(k.snare, p); break;
            case kHat: hatOn(k.hat, variant); break;
            case kCymbal: cymOn(k.cym); break;
            default: percOn(k.perc, p, k.seed); break;
        }
        enforceBudget(k, p, d);
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)v;
        (void)p;   // one-shot drums: the gate does not matter
    }

    bool setSendTap(int slot, float* delay, float* reverb, float gainD, float gainR) override {
        perc::setSendTap(slot, delay, reverb, gainD, gainR);
        return true;
    }

    bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override {
        Kit& k = kitOf(v);
        PercVoice& pv = v.as<PercVoice>();
        if (k.owner != &v || pv.token != k.token) return false;   // handed over
        if (p.moving) enforceBudget(k, p, -1);   // a macro moved a model / FX parameter
        // Kit master: the framework's gain relative to the last hit's
        // velocity (each hit carries its own velocity), so a Volume FX on an
        // empty step scales the ringing kit; the steal / cut fade (explicit
        // flag) ramps it to 0. 1 on the plain path (no extra pass).
        const float gEnd = g0 + gInc * float(kBlock);
        const bool fading = v.fading;
        const float m0 = pv.master;
        float m1 = fading ? 0.0f : (pv.vLast > 1.0e-6f ? clampf(gEnd / pv.vLast, 0.0f, 2.0f) : 1.0f);
        if (std::fabs(m1 - 1.0f) < 1.0e-4f) m1 = 1.0f;   // ramp rounding: the plain path
        pv.master = m1;
        const bool mastered = fading || m0 != 1.0f || m1 != 1.0f;

        // Output stages: 0 Main -> waveshaper -> 1 Post Overdrive -> EQ -> 2 Post EQ.
        const float wsLevel = P(p, kWsLevel);
        const bool wsOn = wsLevel > 0.0f;
        const float eqL = P(p, kEqLow), eqM = P(p, kEqMid), eqH = P(p, kEqHigh);
        const bool eqOn = eqL != 0.0f || eqM != 0.0f || eqH != 0.0f;
        // Stages before the last active FX use the kit bus; a fade uses it for all.
        const int busStages = mastered ? 3 : (eqOn ? 2 : (wsOn ? 1 : 0));
        alignas(16) float acc[2][kBlock];
        alignas(16) float buf[kBlock];
        float* const accP[2] = {acc[0], acc[1]};
        float* const slotP[2] = {L, R};
        if (busStages) {
            zeroWords(acc[0], sizeof(acc[0]));
            zeroWords(acc[1], sizeof(acc[1]));
        }
        const int tapSlot = v.slot < kSynthSlots ? v.slot : 0;
        float* const tapD = gTapD[tapSlot];
        float* const tapR = gTapR[tapSlot];
        const bool tapped = (tapD || tapR) && kitSends(p);
        float wSum = 0.0f, wD = 0.0f, wR = 0.0f;
        const float b0 = v.bendStart, b1 = v.bend;
        float level = 0.0f;
        bool any = false;
        int8_t stageOf[kDrums];   // Output of each ringing drum, -1 = silent
        int stages = 0;           // bit mask of the stages in use
        for (int d = 0; d < kDrums; ++d) {
            stageOf[d] = -1;
            if (!headOf(k, d).on) continue;
            stageOf[d] = int8_t(clampf(P(p, kMixer + 5 * d + 4) + 0.5f, 0.0f, 2.0f));
            stages |= 1 << stageOf[d];
        }
        for (int stage = 0; stage < 3; ++stage) {
            float* const* bus = stage < busStages ? accP : slotP;
            for (int d = 0; d < kDrums && (stages >> stage & 1); ++d) {
                if (stageOf[d] != stage) continue;
                Head& h = headOf(k, d);
                const int mx = kMixer + 5 * d;
                const float g = P(p, mx) * h.vel, pan = P(p, mx + 1);
                Out o{bus[0], bus[1], g * (pan > 0.0f ? 1.0f - pan : 1.0f), g * (pan < 0.0f ? 1.0f + pan : 1.0f)};
                bool on;
                switch (d) {
                    case kKick: on = kickRender(k.kick, p, b0, b1, k.seed, buf, o); break;
                    case kTom: on = tomRender(k.tom, p, b0, b1, k.seed, buf, o); break;
                    case kSnare: on = snareRender(k.snare, p, b0, b1, k.seed, buf, o); break;
                    case kHat: on = hatRender(k.hat, p, k.seed, o); break;
                    case kCymbal: on = cymRender(k.cym, p, k.seed, o); break;
                    default: on = percRender(k.perc, p, k.seed, buf, o); break;
                }
                h.on = on ? 1 : 0;
                const float lv = h.lvl * h.vel;
                if (lv > level) level = lv;
                any = any || on;
                if (tapped) {   // send weights: the drum's level at the block end
                    const float w = lv * P(p, mx);
                    wSum += w;
                    wD += w * P(p, mx + 2);
                    wR += w * P(p, mx + 3);
                }
            }
            // FX after this stage; the last FX before the slot bus writes into it.
            if (stage == 0 && wsOn)
                waveshaper(k, int32_t(clampf(P(p, kWsType) + 0.5f, 0.0f, 4.0f)), wsLevel, accP,
                           busStages == 1 ? slotP : nullptr);
            if (stage == 1 && eqOn) eq(k, eqL, eqM, eqH, accP, busStages == 2 ? slotP : nullptr);
        }
        if (mastered) {   // Volume FX on the ringing kit, or the steal fade
            const float dm = (m1 - m0) * kInvBlock;
            float m = m0;
            for (uint32_t i = 0; i < kBlock; ++i) {
                m += dm;
                L[i] += acc[0][i] * m;
                R[i] += acc[1][i] * m;
            }
        }
        if (tapped) {
            const float inv = wSum > 1.0e-9f ? 1.0f / wSum : 0.0f;
            sendTap(k, L, R, tapD, tapR, wD * inv * gTapGD[tapSlot], wR * inv * gTapGR[tapSlot]);
        } else {
            k.sendG[0] = k.sendG[1] = 0.0f;
        }
        v.level = level > 1.0f ? 1.0f : level;
        if (fading || !any) {   // the kit is silent (or cut by a steal): free it
            kitSilence(k);
            return false;
        }
        return true;
    }
};

PercEngine gPerc;

}  // namespace

Drum drumForNote(uint8_t note, int* hatVariant) {
    if (hatVariant) *hatVariant = 0;
    switch (note) {
        case 60: return kKick;
        case 62: return kSnare;
        case 64:
        case 65:
        case 66:
        case 67:
            if (hatVariant) *hatVariant = note - 64;
            return kHat;
        case 69: return kCymbal;
        case 71: return kPerc;
        case 61:
        case 63:
        case 68:
        case 70: return kNone;
        default: return kTom;
    }
}

void setSendTap(int slot, float* delay, float* reverb, float gainD, float gainR) {
    if (slot < 0 || slot >= kSynthSlots) return;
    gTapD[slot] = delay;
    gTapR[slot] = reverb;
    gTapGD[slot] = delay ? gainD : 0.0f;
    gTapGR[slot] = reverb ? gainR : 0.0f;
}

int slotStateBytes() { return int(sizeof(Kit)); }

int32_t kitCost(SynthSlotScratch& slot, const SynthParams& p, int* ringing) {
    return kitCostOf(slot.as<Kit>(), p, ringing);
}

}  // namespace perc

ISynthEngine* percEngine() { return &perc::gPerc; }

}  // namespace audio
}  // namespace ot
