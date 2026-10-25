// OP-2 (docs/OPTIMIZATION_2X.md): the block-pass reverb (effects.cpp) must
// produce bit-identical output to the per-sample reverb it replaced. This
// file keeps that per-sample implementation (2.x c5652d1) as the reference
// and compares both over loud, silent (bypass / quiet tracking), pre-delay
// and parameter-change scenarios.
#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "audio/dsp.h"
#include "audio/effects.h"
#include "audio/ring_io.h"
#include "test.h"

namespace ot {
namespace audio {
#ifdef OT_AUDIO_SDRAM_STATS
uint32_t gSdramAccesses = 0;
#endif
}  // namespace audio
}  // namespace ot

using namespace ot::audio;

namespace {

constexpr uint32_t kRMask = kReverbRingLen - 1;
inline uint32_t mag(int32_t v) { return uint32_t(v ^ (v >> 31)); }

// The pre-OP-2 reverb, verbatim apart from the class name.
class RefReverb {
public:
    static constexpr uint32_t kMemorySamples = 2 * kReverbRingLen;
    void init(int16_t* mem);
    void clear();
    void setSize(float v) { krt_ = clamp01(v); }
    void setDamping(float v) { klp_ = 1.0f - clamp01(v); }
    void setDiffusion(float v) { kap_ = clamp01(v); }
    void setPredelay(float v) { predelay_ = int32_t(clamp01(v) * 5000.0f); }
    void setAmount(float v) { amount_ = v; }
    void setInputGain(float v) { gain_ = v; }
    bool process(const float* in, float* outL, float* outR);
    float level() const { return level_; }
    bool bypassed() const { return bypass_; }
    static constexpr int32_t kQuiet = 3;

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    int16_t* a_ = nullptr;
    int16_t* b_ = nullptr;
    uint32_t p_ = 0;
    float krt_ = 0.7f, kap_ = 0.625f, klp_ = 0.8f;
    float amount_ = kReverbDefaultAmount, gain_ = kReverbDefaultGain;
    int32_t predelay_ = 0;
    float lp1_ = 0.0f, lp2_ = 0.0f, lp3_ = 0.0f;
    float lfoS_ = 0.0f, lfoPrev_ = 0.0f, lfoC_ = 0.0f;
    float level_ = 0.0f;
    uint32_t quiet_ = 0;
    bool bypass_ = false;
    int32_t prevPre_ = 0;
    static constexpr uint32_t kApLen = 2048;
    int16_t ap_[kApLen] = {};
    void stepLfo(uint32_t p);
};

void RefReverb::init(int16_t* mem) {
    a_ = mem;
    b_ = mem + kReverbRingLen;
    // LFO: s[n+1] = c*s[n] - s[n-1], c ~ 1.99999, updated every 32 samples.
    lfoC_ = 1.99999f;
    clear();
}

void RefReverb::clear() {
    if (a_) std::memset(a_, 0, sizeof(int16_t) * kMemorySamples);
    std::memset(ap_, 0, sizeof(ap_));
    lp1_ = lp2_ = lp3_ = 0.0f;
    const float w = std::acos(lfoC_ * 0.5f);
    lfoPrev_ = 0.0f;
    lfoS_ = std::sin(w);
    p_ = 0;
    level_ = 0.0f;
    quiet_ = kReverbRingLen;   // an empty ring is silent
    bypass_ = false;
}

namespace {
// Reference block buffers (SRAM). Taps are read as 128-sample spans at fixed
// offsets from the block's start pointer P: sample i (pointer P - i) uses
// index 127 - i of a span read from P + offset - 127. Allpass spans are
// updated in place (the read of index k precedes its write).
constexpr uint32_t kModSpan = kBlock + 210;   // modulated del2 read (+-100 + 1)
int16_t gTap[5][kBlock];      // dap1a, dap1b, del1 tap, dap2a, dap2b
int16_t gDel1[kBlock], gDel2[kBlock], gPre[kBlock], gPd[kBlock];
int16_t gMod[kModSpan];
int32_t gModIdx[kBlock];
float gModFrac[kBlock];
}  // namespace

void RefReverb::stepLfo(uint32_t p) {
    if ((p & 31u) == 0) {
        const float s = lfoC_ * lfoS_ - lfoPrev_;
        lfoPrev_ = lfoS_;
        lfoS_ = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    }
}

bool RefReverb::process(const float* in, float* outL, float* outR) {
    if (!a_) {
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        return false;
    }
    int16_t* A = a_;
    int16_t* B = b_;
    const uint32_t P = p_;
    const float kap = kap_, klp = klp_, krt = krt_, amount = amount_, gain = gain_;
    const int32_t pre = predelay_ < 0 ? 4999 : predelay_;
    // Pre-delay input (written before its read, spec order) and silence check.
    bool anyIn = false;
    uint32_t q = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float L = in[i], R = in[i];
        anyIn |= L != 0.0f;
        const int16_t v = toS16((L + R) * gain);
        gPre[kBlock - 1 - i] = v;
        q |= mag(v);
    }
    constexpr float kQuietF = float(kQuiet) / 32768.0f;
    auto small = [&](float v) { return v <= kQuietF && v >= -kQuietF; };
    if (!anyIn && quiet_ >= kReverbRingLen && small(lp1_) && small(lp2_) && small(lp3_)) {
        // Bypass: the whole ring holds +-kQuiet residue only.
        bypass_ = true;
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        uint32_t p = P;
        for (uint32_t i = 0; i < kBlock; ++i) {
            stepLfo(p);
            p = (p - 1u) & kRMask;
        }
        p_ = p;
        lp1_ = lp2_ = lp3_ = 0.0f;
        level_ = 0.0f;
        if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
        return false;
    }
    bypass_ = false;
    // LFO and the modulated del2 read positions (relative to P).
    // (The LFO steps every 32 samples, so the offset is piecewise constant.)
    int32_t lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
    {
        int32_t oi = 0;
        float of = 0.0f;
        uint32_t p = P;
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (i == 0 || (p & 31u) == 0) {
                if (i > 0) lo = oi - int32_t(i - 1) < lo ? oi - int32_t(i - 1) : lo;   // segment end
                stepLfo(p);
                const float off = 4724.0f + 6211.0f + 100.0f * (lfoS_ + 0.5f);
                const uint32_t u = uint32_t(off);
                oi = int32_t(u);
                of = off - float(u);
                hi = oi - int32_t(i) > hi ? oi - int32_t(i) : hi;                     // segment start
            }
            gModIdx[i] = oi - int32_t(i);
            gModFrac[i] = of;
            p = (p - 1u) & kRMask;
        }
        lo = oi - int32_t(kBlock - 1) < lo ? oi - int32_t(kBlock - 1) : lo;
        p_ = p;
    }
    uint32_t modN = uint32_t(hi - lo + 2);
    if (modN > kModSpan) modN = kModSpan;   // cannot happen (|lfo| <= 1)
    // Reads (all before the block's writes: every other tap pair is >= 128
    // samples apart or reads the older value).
    auto tapIn = [&](const int16_t* ring, uint32_t off, int16_t* buf) {
        ringIn(ring, kRMask, P + off - (kBlock - 1), buf, kBlock);
    };
    tapIn(A, 3395, gTap[0]);
    tapIn(A, 6086, gTap[1]);
    tapIn(A, 10588, gTap[2]);
    tapIn(B, 2524, gTap[3]);
    tapIn(B, 4722, gTap[4]);
    ringIn(B, kRMask, P + uint32_t(lo), gMod, modN);
    // Pre-delay: write the block, then read it back pre samples later. The
    // pre-delay region (offsets >= 10590) is read by nothing else and is
    // rewritten by ap1 before the pointer comes round, so with no pre-delay
    // the read returns exactly the block just written: skip both accesses.
    const int16_t* pd = gPre;
    if (pre != 0 && prevPre_ == 0) {
        // Pre-delay switched on: the region was not kept up to date; it
        // starts silent (the first pre samples read zeros).
        ringZero(A, kRMask, P + 10591u, 5000u + kBlock);
    }
    prevPre_ = pre;
    if (pre != 0) {
        ringOut(A, kRMask, P + 10590u - (kBlock - 1), gPre, kBlock);
        tapIn(A, 10590u + uint32_t(pre), gPd);
        pd = gPd;
    }

    float lp1 = lp1_, lp2 = lp2_, lp3 = lp3_;
    float sumAbs = 0.0f;
    // Quietness is tracked only for a silent input (otherwise quiet_ resets).
    auto run = [&](auto trackTag) {
    constexpr bool kTrack = decltype(trackTag)::value;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const uint32_t k = kBlock - 1 - i;
        const float L = in[i], R = in[i];
        lp1 += klp * (s16ToF(pd[k]) - lp1);
        float acc = lp1;
        // Four diffusing all-passes.
        // Four diffusing all-passes (SRAM ring ap_, per sample as in spec order).
        const uint32_t pi = P - i;
        static constexpr uint16_t kApBase[4] = {0, 151, 366, 686};
        static constexpr uint16_t kApTail[4] = {149, 364, 684, 1212};
#pragma GCC unroll 4
        for (int a = 0; a < 4; ++a) {
            const float t = s16ToF(ap_[(pi + kApTail[a]) & (kApLen - 1)]);
            acc += kap * t;
            const int16_t v = toS16(acc);
            ap_[(pi + kApBase[a]) & (kApLen - 1)] = v;
            if (kTrack) q |= mag(v);
            acc = t - kap * acc;
        }
        const float apout = acc;
        // Left tank: modulated read of del2 (ring B).
        {
            const int32_t m = gModIdx[i] - lo;
            const float a0 = s16ToF(gMod[m]), a1 = s16ToF(gMod[m + 1]);
            acc = apout + krt * (a0 + (a1 - a0) * gModFrac[i]);
        }
        lp2 += klp * (acc - lp2);
        acc = lp2;
        {   // dap1a: read -kap, write-allpass +kap
            const float t = s16ToF(gTap[0][k]);
            acc += -kap * t;
            const int16_t v = toS16(acc);
            gTap[0][k] = v;
            if (kTrack) q |= mag(v);
            acc = kap * acc + t;
        }
        {   // dap1b: read +kap, write-allpass -kap
            const float t = s16ToF(gTap[1][k]);
            acc += kap * t;
            const int16_t v = toS16(acc);
            gTap[1][k] = v;
            if (kTrack) q |= mag(v);
            acc = t - kap * acc;
        }
        {   // del1
            const int16_t v = toS16(acc);
            gDel1[k] = v;
            if (kTrack) q |= mag(v);
        }
        const float wetL = 2.0f * acc;
        // Right tank: fixed del1 tap.
        acc = apout + krt * (s16ToF(gTap[2][k]));
        lp3 += klp * (acc - lp3);
        acc = lp3;
        {   // dap2a
            const float t = s16ToF(gTap[3][k]);
            acc += kap * t;
            const int16_t v = toS16(acc);
            gTap[3][k] = v;
            if (kTrack) q |= mag(v);
            acc = t - kap * acc;
        }
        {   // dap2b
            const float t = s16ToF(gTap[4][k]);
            acc += -kap * t;
            const int16_t v = toS16(acc);
            gTap[4][k] = v;
            if (kTrack) q |= mag(v);
            acc = kap * acc + t;
        }
        {   // del2
            const int16_t v = toS16(acc);
            gDel2[k] = v;
            if (kTrack) q |= mag(v);
        }
        const float wetR = 2.0f * acc;
        outL[i] = clampUnit(L + amount * (wetL - L));
        outR[i] = clampUnit(R + amount * (wetR - R));
        sumAbs += (wetL < 0.0f ? -wetL : wetL);
    }
    };
    if (anyIn) run(std::false_type{});
    else run(std::true_type{});
    // Writes (disjoint spans).
    auto tapOut = [&](int16_t* ring, uint32_t off, const int16_t* buf) {
        ringOut(ring, kRMask, P + off - (kBlock - 1), buf, kBlock);
    };
    tapOut(A, 1214, gTap[0]);
    tapOut(A, 3397, gTap[1]);
    tapOut(A, 6088, gDel1);
    tapOut(B, 0, gTap[3]);
    tapOut(B, 2526, gTap[4]);
    tapOut(B, 4724, gDel2);
    // Keep the filter states finite and bounded.
    lp1_ = clampUnit(lp1);
    lp2_ = clampUnit(lp2);
    lp3_ = clampUnit(lp3);
    level_ = sumAbs / float(kBlock);
    if (anyIn || q > uint32_t(kQuiet)) quiet_ = 0;
    else if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
    return true;
}

}  // namespace

namespace {
struct Pair {
    std::vector<int16_t> memA = std::vector<int16_t>(Reverb::kMemorySamples), memB = std::vector<int16_t>(Reverb::kMemorySamples);
    std::unique_ptr<Reverb> a{new Reverb};
    std::unique_ptr<RefReverb> b{new RefReverb};
    Pair() {
        a->init(memA.data());
        b->init(memB.data());
    }
    template <class F>
    void both(F f) {
        f(*a);
        f(*b);
    }
    // One block; returns false (and counts a failure) on any difference.
    bool step(const float* in) {
        float l1[kBlock], r1[kBlock], l2[kBlock], r2[kBlock];
        const bool ra = a->process(in, l1, r1);
        const bool rb = b->process(in, l2, r2);
        bool same = ra == rb && a->bypassed() == b->bypassed() &&
                    std::memcmp(&l1, &l2, sizeof(l1)) == 0 && std::memcmp(&r1, &r2, sizeof(r1)) == 0;
        const float la = a->level(), lb = b->level();
        same = same && std::memcmp(&la, &lb, sizeof(la)) == 0;
        return same;
    }
};

void signal(float* x, uint32_t block, int kind, float amp) {
    uint32_t s = block * 2654435761u + 7u;
    for (uint32_t i = 0; i < kBlock; ++i) {
        s = s * 1664525u + 1013904223u;
        const float n = float(int32_t(s) >> 8) * (1.0f / 8388608.0f);
        const float t = float(block * kBlock + i);
        switch (kind) {
            case 0: x[i] = 0.0f; break;
            case 1: x[i] = amp * std::sin(t * 0.031f); break;
            case 2: x[i] = amp * n; break;
            default: x[i] = (i & 63) == 0 ? amp : 0.0f; break;   // clicks
        }
    }
}
}  // namespace

TEST(reverb_blocks_match_reference) {
    Pair p;
    float in[kBlock];
    uint32_t bad = 0, blk = 0;
    auto run = [&](int kind, float amp, int blocks) {
        for (int k = 0; k < blocks; ++k, ++blk) {
            signal(in, blk, kind, amp);
            if (!p.step(in)) ++bad;
        }
    };
    run(1, 0.3f, 300);                 // loud sine, default settings
    run(0, 0.0f, 400);                 // tail into silence: quiet tracking + bypass
    run(3, 0.9f, 50);                  // clicks wake it up
    p.both([](auto& r) { r.setPredelay(0.37f); });
    run(2, 0.5f, 200);                 // pre-delay switched on, noise
    p.both([](auto& r) { r.setPredelay(0.0f); });
    run(1, 1.5f, 100);                 // pre-delay off, clipping input
    p.both([](auto& r) {
        r.setSize(1.0f);
        r.setDamping(0.0f);
        r.setDiffusion(1.0f);
        r.setAmount(1.0f);
        r.setInputGain(2.0f);
    });
    run(2, 0.8f, 300);                 // extreme feedback, saturating rings
    p.both([](auto& r) { r.setPredelay(1.0f); });
    run(3, 1.0f, 200);
    p.both([](auto& r) {
        r.setSize(0.2f);
        r.setDamping(0.9f);
        r.setPredelay(0.0f);
        r.setAmount(0.3f);
        r.setInputGain(0.5f);
    });
    run(1, 0.2f, 100);
    run(0, 0.0f, 600);                 // long silence (bypass again)
    run(1, 0.4f, 50);
    std::printf("  %u blocks compared, %u differ\n", blk, bad);
    CHECK_EQ(bad, 0u);
}
