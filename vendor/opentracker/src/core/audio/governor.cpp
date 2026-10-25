// Synth voice governor (governor.h).
#include "audio/governor.h"

#include "audio/cpu_meter.h"

namespace ot {
namespace audio {

void Governor::setBudget(int b) {
    budget_ = b < 0 ? 0 : (b > kMaxBudget ? kMaxBudget : b);
    reset();
}

void Governor::reset() {
    cap_ = budget_;
    holdOff_ = 0;
    calm_ = 0;
    sinceRisk_ = 0xFFFFFFFFu;
}

void Governor::cutTo(int c) {
    if (c < 0) c = 0;
    if (c > cap_) c = cap_;
    cap_ = c;
    holdOff_ = kHoldOffBlocks;
    calm_ = 0;
    sinceCut_ = 0;
    ++cuts_;
    // Steal until the sounding count fits (bounded: one per voice).
    for (int n = pool_->sounding(), i = 0; n > cap_ && i <= kMaxBudget; n = pool_->sounding(), ++i) {
        if (!pool_->stealOne()) break;
        ++steals_;
    }
}

void Governor::evaluate(uint32_t l, uint32_t a, bool overrun) {
    if (!enabled_ || !pool_) return;
    if (sinceCut_ != 0xFFFFFFFFu) ++sinceCut_;
    if (holdOff_) --holdOff_;
    // Near-overrun blocks: one >= 980, or the second >= 950 within 1 s.
    bool risk = false;
    if (l >= kRiskPermille) {
        risk = l >= kRiskNowPermille || sinceRisk_ < kRiskWindowBlocks;
        sinceRisk_ = 0;
    } else if (sinceRisk_ != 0xFFFFFFFFu) {
        ++sinceRisk_;
    }
    const int sounding = pool_->sounding();
    if (sounding > 0 && holdOff_ == 0) {
        const int fl = floor();
        const int cur = cap_ < sounding ? cap_ : sounding;
        if (overrun || l >= kOverrunPermille) {
            cutTo(sounding - 1);
            return;
        }
        if (risk) {
            const int to = cur - 1 > fl ? cur - 1 : fl;   // max(F, min(C, sounding) - 1)
            if (to < cap_) {
                cutTo(to);
                return;
            }
        }
        if (a >= kDeepPermille && cap_ > 0) {
            cutTo(cap_ - 1);
            return;
        }
        if (a >= kCutPermille && cap_ > fl) {
            cutTo(cap_ - 1);
            return;
        }
    }
    if (a < kRestorePermille && l < kCalmPeakPermille) {
        if (cap_ < budget_ && ++calm_ >= kRestoreBlocks) {
            ++cap_;
            calm_ = 0;
        }
    } else {
        calm_ = 0;
    }
}

void Governor::evaluate(const CpuMeter& m) { evaluate(m.last(), m.avg(), m.overrunLast()); }

}  // namespace audio
}  // namespace ot
