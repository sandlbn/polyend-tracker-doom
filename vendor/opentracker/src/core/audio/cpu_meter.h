// Audio render load meter (docs/specs/2x-02 §1, §4; 2.x M1 plan §5) [OT].
//
// The engine's live fill callback (AudioEngine::fillCallback) reads a cycle
// counter before and after each 128-frame block and feeds the difference to
// addBlock(). The counter is injected: the device installs the DWT CYCCNT
// (setCycleSource, hal/board.cpp), host tests a fake; without a source the
// meter stays empty (hasData() false) and the UI cell stays hidden. The HAL's
// own render window (IAudioOut renderCyclesWindow, /.develop overlay and the
// 5 s telemetry line) is independent of this meter.
//
// Values, in permille of the block budget (522 240 cycles at 180 MHz):
//   L  last block;
//   A  exponential average, alpha = 1/32 per block (~100 ms);
//   P  max of L over a 1 s window (345 blocks), held for one more window
//      (a spike stays visible 1..2 s);
//   overrun: a block with L >= 1000 (no HAL miss counter: L >= 100 % proxy).
// Written from the audio context, read from the main loop: every field the
// readers use is a naturally aligned 32-bit word (atomic on Cortex-M4).
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

class CpuMeter {
public:
    using CycleFn = uint32_t (*)();
    static constexpr uint32_t kBudgetCycles = 522240;    // 128 / 44117.6 Hz * 180 MHz
    static constexpr uint32_t kWindowBlocks = 345;       // ~1 s of 2.90 ms blocks
    static constexpr uint32_t kRecentBlocks = 690;       // ~2 s ("in the last 2 s")
    static constexpr uint32_t kOverrunPermille = 1000;

    // Global cycle counter (device: DWT CYCCNT). nullptr: none (meter idle).
    static void setCycleSource(CycleFn fn);
    static CycleFn cycleSource();
    // Current counter value, 0 without a source.
    static uint32_t cycles();
    // Counter for the block profile's phase stamps (block_profile.h). The
    // cycle source by default; setStampSource after setCycleSource changes
    // it (tests with a fake per-block counter turn the stamps off: nullptr).
    static void setStampSource(CycleFn fn);
    static uint32_t stampCycles();

    // One rendered block took `cycles` cycles.
    void addBlock(uint32_t cycles);
    // Clears all values and counters.
    void reset();

    bool hasData() const { return blocks_ != 0; }
    uint32_t last() const { return last_; }               // L, permille
    uint32_t avg() const { return avgQ_ >> 5; }           // A, permille
    uint32_t peak() const { return winMax_ > prevMax_ ? winMax_ : prevMax_; }   // P, permille
    bool overrunLast() const { return last_ >= kOverrunPermille; }
    uint32_t overruns() const { return overruns_; }
    uint32_t blocks() const { return blocks_; }
    // The last addBlock() closed a 1 s window (block_profile.h rolls with it).
    bool windowEnded() const { return blocks_ != 0 && winLeft_ == kWindowBlocks; }
    // An overrun in the last ~2 s (kRecentBlocks).
    bool overrunRecent() const { return overruns_ && blocks_ - lastOverrun_ < kRecentBlocks; }

    static uint32_t permille(uint32_t cycles) {
        return uint32_t(uint64_t(cycles) * 1000u / kBudgetCycles);
    }

private:
    uint32_t last_ = 0;
    uint32_t avgQ_ = 0;        // A << 5
    uint32_t winMax_ = 0;      // max L in the current window
    uint32_t prevMax_ = 0;     // max L of the previous window (the hold)
    uint32_t winLeft_ = kWindowBlocks;
    uint32_t blocks_ = 0;
    uint32_t overruns_ = 0;
    uint32_t lastOverrun_ = 0;
};

}  // namespace audio
}  // namespace ot
