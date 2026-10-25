// Auto-slice transient detector (spec 13 §F, exact algorithm).
#include "app/auto_slice.h"

#include <cstring>

namespace ot {

namespace {
double bitsToDouble(uint64_t b) {
    double d;
    std::memcpy(&d, &b, sizeof(d));
    return d;
}

// Sum of squares of x[0..n) as u64.
uint64_t sumSquares(const int16_t* x, uint32_t n) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += uint64_t(int32_t(x[i]) * int32_t(x[i]));
    return s;
}
}  // namespace

float AutoSlicer::threshold(const uint32_t* e, int n) {
    // spec 13 §F.4: 64-bit sums, low 32 bits of mean and variance kept.
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += e[i];
    const uint32_t mean = uint32_t(sum / uint64_t(n));
    uint64_t vs = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t d = int64_t(e[i]) - int64_t(mean);
        vs += uint64_t(d * d);
    }
    const uint32_t var = uint32_t(vs / uint64_t(n));
    static const double kA = bitsToDouble(0x3FFA127913E81451ull);   // 1.62951...
    static const double kB = bitsToDouble(0xBDE5798EE2308C3Aull);   // -1.5625e-10
    double c = kA + kB * double(var);
    if (c < 1.2) c = 1.2;
    return float(mean) * float(c);
}

bool AutoSlicer::start(const int16_t* data, uint32_t len, uint16_t* out, uint8_t* outCount) {
    if (!data || !out || len < kMinLength) return false;   // §F.2
    data_ = data;
    len_ = len;
    out_ = out;
    outCount_ = outCount;
    active_ = true;
    pos_ = 0;
    last_ = 0;
    count_ = 0;
    idx_ = 0;
    final_ = false;
    for (uint32_t& r : ring_) r = 0;
    return true;
}

void AutoSlicer::append(uint32_t p) {
    // §F.5: u16((65535 / len) * p), float, truncated.
    const float f = (65535.0f / float(len_)) * float(p);
    out_[count_++] = uint16_t(f >= 65535.0f ? 65535.0f : f);
}

uint32_t AutoSlicer::refine(uint32_t p) const {
    // spec 13 §F.6. The window is clamped to the sample end (the stock
    // window may run up to ~6144 frames past it; §F.6 recommends clamping).
    uint32_t s, m;
    if (p < kHalfWindow) {
        s = 0;
        m = ((2 * kHalfWindow - p) >> 7) & 0xFFFF;
    } else if (p + kHalfWindow > len_) {
        s = p - kHalfWindow;
        m = ((p - len_ + 2 * kHalfWindow) >> 7) & 0xFFFF;
    } else {
        s = p - kHalfWindow;
        m = 96;
    }
    if (s < len_ && s + m * kSub > len_) m = (len_ - s) / kSub;
    if (m <= 5) return s;
    uint32_t e[96];
    if (m > 96) m = 96;
    for (uint32_t k = 0; k < m; ++k) e[k] = uint32_t(sumSquares(data_ + s + k * kSub, kSub) >> 7);
    const float thr = threshold(e, int(m));
    for (uint32_t k = 5; k < m; ++k)
        if (float(e[k]) > thr && (count_ == 0 || s + kSub * k > last_ + kSpacing)) return s + kSub * (k - 5);
    return s;
}

bool AutoSlicer::step() {
    if (!active_) return false;
    // §F.3 block energy.
    uint32_t n = kBlock;
    if (len_ - pos_ < kBlock) {
        n = len_ - pos_;
        final_ = true;
    }
    // The stock sum covers all 2048 scratch entries (stale squares in a short
    // final block); that block is never tested, so only full blocks matter.
    const uint32_t energy = uint32_t(sumSquares(data_ + pos_, n) >> 11);
    // §F.5 detection, before the energy enters the ring.
    if (pos_ == kRing * kBlock) {
        const float thr = threshold(ring_, kRing);
        for (int i = 0; i < kRing; ++i) {
            const uint32_t bp = uint32_t(i) * kBlock;
            if (float(ring_[i]) > thr && (i == 0 || bp > last_ + kSpacing) && count_ < kMaxSlices) {
                last_ = bp;
                append(refine(bp));
            }
        }
        if (count_ == 0) append(0);
    } else if (pos_ > kRing * kBlock && !final_) {
        const float thr = threshold(ring_, kRing);
        if (float(energy) > thr && pos_ > last_ + kSpacing && count_ < kMaxSlices) {
            const uint32_t p = refine(pos_);
            last_ = p;
            append(p);
        }
    }
    ring_[idx_] = energy;
    idx_ = uint8_t((idx_ + 1) % kRing);
    pos_ += n;
    if (pos_ >= len_ || count_ >= kMaxSlices) {
        active_ = false;
        if (outCount_) *outCount_ = count_;
    }
    return active_;
}

}  // namespace ot
