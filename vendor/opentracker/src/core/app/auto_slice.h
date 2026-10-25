// Auto-slice transient detector (spec 13 §F). Runs incrementally: start()
// arms it, step() processes one 2048-frame block and is called once per
// main-loop pass while busy() (spec 13 §F.1), so the UI stays live.
#pragma once

#include <cstdint>

namespace ot {

class AutoSlicer {
public:
    static constexpr uint32_t kBlock = 2048;        // §F.3
    static constexpr int kRing = 5;                 // §F.5
    static constexpr uint32_t kMinLength = 10240;   // §F.2: shorter samples get no slices
    static constexpr uint32_t kSpacing = 10240;     // §F.5 minimum distance to the last slice
    static constexpr uint32_t kSub = 128;           // §F.6 sub-block
    static constexpr uint32_t kHalfWindow = 6144;   // §F.6
    static constexpr int kMaxSlices = 48;

    // Arm the detector on mono/left int16 data. Slice positions go to
    // out[0..47] (u16 fractions of len), the count to *outCount when done.
    // Returns false (nothing happens, count stays as it is) when
    // len < kMinLength (§F.2).
    bool start(const int16_t* data, uint32_t len, uint16_t* out, uint8_t* outCount);
    // Process one block. Returns busy().
    bool step();
    bool busy() const { return active_; }
    // Run to completion.
    void finish() {
        while (step()) {
        }
    }
    void cancel() { active_ = false; }
    uint8_t count() const { return count_; }

    // §F.4: onset threshold over n energies (mean * C, C in [1.2, 1.6295]).
    static float threshold(const uint32_t* e, int n);

private:
    uint32_t refine(uint32_t p) const;
    void append(uint32_t p);

    bool active_ = false;
    uint32_t pos_ = 0;
    uint8_t idx_ = 0;
    const int16_t* data_ = nullptr;
    uint16_t* out_ = nullptr;
    uint8_t* outCount_ = nullptr;
    uint32_t len_ = 0;
    uint32_t ring_[kRing] = {};
    uint32_t last_ = 0;
    bool final_ = false;
    uint8_t count_ = 0;
};

}  // namespace ot
