// Sequencer random generator (spec 03 §3.6 "Random generator").
//
// Park-Miller minimal standard generator (multiplier 16807, modulus 2^31-1,
// Schrage decomposition q = 127773, r = 2836). A zero state is replaced by the
// seed 0x1f0cce42 before use.
#pragma once

#include <cstdint>

namespace ot {
namespace seq {

class ParkMiller {
public:
    static constexpr uint32_t kDefaultSeed = 0x1f0cce42u;  // spec 03 §3.6

    explicit ParkMiller(uint32_t seed = 0) : state_(seed) {}

    void seed(uint32_t s) { state_ = s; }
    uint32_t state() const { return state_; }

    // Next raw value in 1..2^31-2.
    uint32_t next() {
        if (state_ == 0) state_ = kDefaultSeed;
        const int32_t s = int32_t(state_);
        const int32_t hi = s / 127773;
        const int32_t lo = s % 127773;
        int32_t t = 16807 * lo - 2836 * hi;
        if (t <= 0) t += 0x7fffffff;
        state_ = uint32_t(t);
        return state_;
    }

    // rand[a, b): a + (s mod (b - a)); returns a when b <= a (spec 03 §3.6).
    int32_t range(int32_t a, int32_t b) {
        if (b <= a) return a;
        return a + int32_t(next() % uint32_t(b - a));
    }

    // Inclusive helper for the spec's closed-interval notation rand[a, b].
    int32_t rangeIncl(int32_t a, int32_t b) { return range(a, b + 1); }

private:
    uint32_t state_;
};

}  // namespace seq
}  // namespace ot
