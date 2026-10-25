// OpenTracker NES core: APU (RP2A03 sound), NTSC. Channel behaviour per the
// public NESdev APU documentation. Each output sample is the average of the
// mixed channel levels over its CPU cycles (box filter), then a DC-blocking
// high-pass; mono, unity gain (spec 14 §4).
#include "games/nes.h"

namespace ot {
namespace games {

namespace {
constexpr uint8_t kLength[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                 12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint8_t kDuty[4] = {0x02, 0x06, 0x1E, 0xF9};   // sequence bits, step 0 = bit 0
constexpr uint8_t kTriangle[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                   0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr uint16_t kNoise[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
constexpr uint16_t kDmcRate[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
// Frame sequencer steps (CPU cycles after the $4017 write).
constexpr int32_t kStep1 = 7457, kStep2 = 14913, kStep3 = 22371, kStep4 = 29829, kStep5 = 37281;
// Linear mixer approximation (NESdev "APU Mixer": 0.00752 pulse, 0.00851
// triangle, 0.00494 noise, 0.00335 DMC per output step), in units of 1e-5 so
// the per-sample integral stays in int32 (40 cycles x 85,280 max).
constexpr int32_t kPulseGain = 752, kTriGain = 851, kNoiseGain = 494, kDmcGain = 335;
constexpr float kOutGain = 40000.0f * 1e-5f;   // full-scale mix (~0.85) -> just under clipping after the high-pass
constexpr float kHighPass = 0.996f;    // ~28 Hz DC blocker at 44.1 kHz
}  // namespace

void Nes::setSampleRate(uint32_t hz) {
    if (hz < 8000) hz = 8000;
    samplePeriod16_ = int32_t((int64_t(kCpuHz) << 16) / hz);
}

void Nes::Envelope::clock() {
    if (start) {
        start = false;
        decay = 15;
        divider = period;
    } else if (divider == 0) {
        divider = period;
        if (decay) --decay;
        else if (loop) decay = 15;
    } else {
        --divider;
    }
}

uint16_t Nes::Pulse::target() const {
    const uint16_t change = uint16_t(timer >> sweepShift);
    if (sweepNeg) return uint16_t(timer - change - (ones ? 1 : 0));
    return uint16_t(timer + change);
}

bool Nes::Pulse::muted() const { return timer < 8 || (!sweepNeg && target() > 0x7FF); }

int Nes::Pulse::out() const {
    if (!length || muted() || !((kDuty[duty] >> seq) & 1)) return 0;
    return env.volume();
}

int32_t Nes::Pulse::integrate(int32_t n) {
    // Sequencer steps every 2 * (timer + 1) CPU cycles. A silent pulse (muted
    // by the sweep unit, timer < 8, or volume 0) is not stepped: its phase is
    // inaudible and games park unused channels at tiny periods.
    if (!env.volume() || muted()) return 0;
    int o = out();
    if (n < timerLeft) {   // no step inside the span (the common case)
        timerLeft -= n;
        return o * n;
    }
    int32_t sum = 0;
    while (n > 0) {
        const int32_t k = n < timerLeft ? n : timerLeft;
        sum += o * k;
        n -= k;
        timerLeft -= k;
        if (timerLeft == 0) {
            timerLeft = 2 * (int32_t(timer) + 1);
            seq = uint8_t((seq + 1) & 7);
            o = out();
        }
    }
    return sum;
}

int32_t Nes::Triangle::integrate(int32_t n) {
    // Steps every timer + 1 CPU cycles while both counters are non-zero.
    // Periods < 2 (ultrasonic) hold the output, as games use them to mute.
    const bool run = length && linear && timer >= 2;
    int o = kTriangle[seq];
    if (!run) return o * n;
    if (n < timerLeft) {
        timerLeft -= n;
        return o * n;
    }
    int32_t sum = 0;
    while (n > 0) {
        const int32_t k = n < timerLeft ? n : timerLeft;
        sum += o * k;
        n -= k;
        timerLeft -= k;
        if (timerLeft == 0) {
            timerLeft = int32_t(timer) + 1;
            seq = uint8_t((seq + 1) & 31);
            o = kTriangle[seq];
        }
    }
    return sum;
}

int32_t Nes::Noise::integrate(int32_t n) {
    if (!env.volume()) return 0;   // silent: the LFSR phase is inaudible
    int o = out();
    if (n < timerLeft) {
        timerLeft -= n;
        return o * n;
    }
    const uint8_t vol = env.volume();
    const int tap = mode ? 6 : 1;
    int32_t sum = o * timerLeft;
    n -= timerLeft;
    // Whole periods: the output is vol while LFSR bit 0 is clear.
    int32_t clear = 0;
    for (;;) {
        const uint16_t fb = uint16_t((lfsr ^ (lfsr >> tap)) & 1);
        lfsr = uint16_t((lfsr >> 1) | (fb << 14));
        if (n < int32_t(period)) break;
        clear += !(lfsr & 1);
        n -= period;
    }
    sum += clear * int32_t(period) * vol;
    o = (lfsr & 1) ? 0 : vol;
    sum += o * n;
    timerLeft = int32_t(period) - n;
    return sum;
}

void Nes::dmcFetch() {
    // Memory reader: refill the sample buffer from CPU space ($8000-$FFFF).
    if (dmc_.bufFull || dmc_.left == 0) return;
    dmc_.buf = prg_[(dmc_.addr >> 13) & 3][dmc_.addr & 0x1FFF];
    dmc_.bufFull = true;
    dmc_.addr = dmc_.addr == 0xFFFF ? 0x8000 : uint16_t(dmc_.addr + 1);
    if (--dmc_.left == 0) {
        if (dmc_.loop) {
            dmc_.addr = dmc_.startAddr;
            dmc_.left = dmc_.startLen;
        } else if (dmc_.irqOn) {
            irq_ |= 2;
        }
    }
}

int32_t Nes::dmcIntegrate(int32_t n) {
    if (n < dmc_.timerLeft) {
        dmc_.timerLeft -= n;
        return dmc_.level * n;
    }
    int32_t sum = 0;
    while (n > 0) {
        const int32_t k = n < dmc_.timerLeft ? n : dmc_.timerLeft;
        sum += dmc_.level * k;
        n -= k;
        dmc_.timerLeft -= k;
        if (dmc_.timerLeft == 0) {
            dmc_.timerLeft = dmc_.rate;
            if (!dmc_.silence) {
                if (dmc_.shift & 1) {
                    if (dmc_.level <= 125) dmc_.level = uint8_t(dmc_.level + 2);
                } else if (dmc_.level >= 2) {
                    dmc_.level = uint8_t(dmc_.level - 2);
                }
            }
            dmc_.shift >>= 1;
            if (--dmc_.bits == 0) {
                dmc_.bits = 8;
                if (dmc_.bufFull) {
                    dmc_.shift = dmc_.buf;
                    dmc_.bufFull = false;
                    dmc_.silence = false;
                    dmcFetch();
                } else {
                    dmc_.silence = true;
                }
            }
        }
    }
    return sum;
}

void Nes::apuQuarter() {
    p1_.env.clock();
    p2_.env.clock();
    noise_.env.clock();
    if (tri_.reloadFlag) tri_.linear = tri_.linearReload;
    else if (tri_.linear) --tri_.linear;
    if (!tri_.control) tri_.reloadFlag = false;
}

void Nes::apuHalf() {
    Pulse* ps[2] = {&p1_, &p2_};
    for (Pulse* p : ps) {
        if (p->length && !p->halt) --p->length;
        if (p->sweepDiv == 0 && p->sweepOn && p->sweepShift && !p->muted()) p->timer = p->target();
        if (p->sweepDiv == 0 || p->sweepReload) {
            p->sweepDiv = p->sweepPeriod;
            p->sweepReload = false;
        } else {
            --p->sweepDiv;
        }
    }
    if (tri_.length && !tri_.control) --tri_.length;
    if (noise_.length && !noise_.halt) --noise_.length;
}

void Nes::emitSample(float v) {
    // v: mean mix level in 1e-5 units. DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1].
    const float y = v - hpPrevIn_ + kHighPass * hpPrevOut_;
    hpPrevIn_ = v;
    hpPrevOut_ = y;
    float s = y * kOutGain;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    if (audioCount_ < kAudioCap) mem_.audio[audioCount_++] = int16_t(s);
}

void Nes::fcSchedule() {
    // Next frame-sequencer event position (fcPos_ resets after the last step).
    if (fcPos_ < kStep1) fcNext_ = kStep1;
    else if (fcPos_ < kStep2) fcNext_ = kStep2;
    else if (fcPos_ < kStep3) fcNext_ = kStep3;
    else if (!fcFive_) fcNext_ = fcPos_ < kStep4 ? kStep4 : kStep4 + 1;
    else fcNext_ = fcPos_ < kStep5 ? kStep5 : kStep5 + 1;
}

void Nes::apuRun(int32_t n) {
    while (n > 0) {
        // Next event: frame-sequencer step or the end of the output sample.
        int32_t k = n;
        if (fcNext_ - fcPos_ < k) k = fcNext_ - fcPos_;
        const int32_t toSample = (sampleLeft16_ + 0xFFFF) >> 16;
        if (toSample < k) k = toSample;
        // Integrate the channels over k cycles (silent channels are skipped;
        // their phase does not matter while they are silent).
        int32_t sum = 0;
        if (p1_.length) sum += kPulseGain * p1_.integrate(k);
        if (p2_.length) sum += kPulseGain * p2_.integrate(k);
        sum += kTriGain * tri_.integrate(k);
        if (noise_.length) sum += kNoiseGain * noise_.integrate(k);
        if (dmc_.level || !dmc_.silence || dmc_.left) sum += kDmcGain * dmcIntegrate(k);
        acc_ += sum;
        accCycles_ += k;
        n -= k;
        fcPos_ += k;
        sampleLeft16_ -= k << 16;
        if (sampleLeft16_ <= 0) {
            sampleLeft16_ += samplePeriod16_;
            emitSample(accCycles_ ? float(acc_) / float(accCycles_) : 0.0f);
            acc_ = 0;
            accCycles_ = 0;
        }
        if (fcPos_ != fcNext_) continue;
        // Frame sequencer.
        if (fcPos_ == kStep1 || fcPos_ == kStep3) {
            apuQuarter();
        } else if (fcPos_ == kStep2) {
            apuQuarter();
            apuHalf();
        } else if (!fcFive_ && fcPos_ == kStep4) {
            apuQuarter();
            apuHalf();
            if (!fcInhibit_) irq_ |= 1;
        } else if (fcFive_ && fcPos_ == kStep5) {
            apuQuarter();
            apuHalf();
        } else {
            fcPos_ = 0;   // kStep4 + 1 (4-step) or kStep5 + 1 (5-step): period end
        }
        fcSchedule();
    }
}

void Nes::apuSync() {
    const uint64_t now = cycles_;
    if (now > apuCycles_) {
        uint64_t n = now - apuCycles_;
        while (n > 0) {
            const int32_t k = n > 0x10000 ? 0x10000 : int32_t(n);
            apuRun(k);
            n -= uint64_t(k);
        }
    }
    apuCycles_ = now;
}

void Nes::apuWrite(uint16_t a, uint8_t v) {
    apuSync();
    switch (a) {
        case 0x4000: case 0x4004: {
            Pulse& p = a == 0x4000 ? p1_ : p2_;
            p.duty = v >> 6;
            p.halt = p.env.loop = v & 0x20;
            p.env.constant = v & 0x10;
            p.env.period = v & 0x0F;
            break;
        }
        case 0x4001: case 0x4005: {
            Pulse& p = a == 0x4001 ? p1_ : p2_;
            p.sweepOn = v & 0x80;
            p.sweepPeriod = (v >> 4) & 7;
            p.sweepNeg = v & 0x08;
            p.sweepShift = v & 7;
            p.sweepReload = true;
            break;
        }
        case 0x4002: case 0x4006: {
            Pulse& p = a == 0x4002 ? p1_ : p2_;
            p.timer = uint16_t((p.timer & 0x700) | v);
            break;
        }
        case 0x4003: case 0x4007: {
            Pulse& p = a == 0x4003 ? p1_ : p2_;
            p.timer = uint16_t((p.timer & 0xFF) | ((v & 7) << 8));
            if (p.enabled) p.length = kLength[v >> 3];
            p.seq = 0;
            p.env.start = true;
            break;
        }
        case 0x4008:
            tri_.control = v & 0x80;
            tri_.linearReload = v & 0x7F;
            break;
        case 0x400A: tri_.timer = uint16_t((tri_.timer & 0x700) | v); break;
        case 0x400B:
            tri_.timer = uint16_t((tri_.timer & 0xFF) | ((v & 7) << 8));
            if (tri_.enabled) tri_.length = kLength[v >> 3];
            tri_.reloadFlag = true;
            break;
        case 0x400C:
            noise_.halt = noise_.env.loop = v & 0x20;
            noise_.env.constant = v & 0x10;
            noise_.env.period = v & 0x0F;
            break;
        case 0x400E:
            noise_.mode = v & 0x80;
            noise_.period = kNoise[v & 0x0F];
            break;
        case 0x400F:
            if (noise_.enabled) noise_.length = kLength[v >> 3];
            noise_.env.start = true;
            break;
        case 0x4010:
            dmc_.irqOn = v & 0x80;
            dmc_.loop = v & 0x40;
            dmc_.rate = kDmcRate[v & 0x0F];
            if (!dmc_.irqOn) irq_ &= uint8_t(~2);
            break;
        case 0x4011: dmc_.level = v & 0x7F; break;
        case 0x4012: dmc_.startAddr = uint16_t(0xC000 | (uint16_t(v) << 6)); break;
        case 0x4013: dmc_.startLen = uint16_t((uint16_t(v) << 4) | 1); break;
        case 0x4015:
            p1_.enabled = v & 1;
            p2_.enabled = v & 2;
            tri_.enabled = v & 4;
            noise_.enabled = v & 8;
            if (!p1_.enabled) p1_.length = 0;
            if (!p2_.enabled) p2_.length = 0;
            if (!tri_.enabled) tri_.length = 0;
            if (!noise_.enabled) noise_.length = 0;
            if (!(v & 0x10)) {
                dmc_.left = 0;
            } else if (dmc_.left == 0) {
                dmc_.addr = dmc_.startAddr;
                dmc_.left = dmc_.startLen;
                dmcFetch();
            }
            irq_ &= uint8_t(~2);
            break;
        case 0x4017:
            fcFive_ = v & 0x80;
            fcInhibit_ = v & 0x40;
            if (fcInhibit_) irq_ &= uint8_t(~1);
            fcPos_ = 0;
            fcSchedule();
            if (fcFive_) {
                apuQuarter();
                apuHalf();
            }
            break;
        default:
            break;
    }
}

uint8_t Nes::apuStatus() {
    apuSync();
    const uint8_t r = uint8_t((p1_.length ? 1 : 0) | (p2_.length ? 2 : 0) | (tri_.length ? 4 : 0) |
                              (noise_.length ? 8 : 0) | (dmc_.left ? 0x10 : 0) | ((irq_ & 1) ? 0x40 : 0) |
                              ((irq_ & 2) ? 0x80 : 0));
    irq_ &= uint8_t(~1);   // reading clears the frame interrupt
    return r;
}

}  // namespace games
}  // namespace ot
