// Audio render load meter (cpu_meter.h).
#include "audio/cpu_meter.h"

namespace ot {
namespace audio {

namespace {
CpuMeter::CycleFn gCycles = nullptr;
CpuMeter::CycleFn gStamp = nullptr;
}

void CpuMeter::setCycleSource(CycleFn fn) { gCycles = gStamp = fn; }
void CpuMeter::setStampSource(CycleFn fn) { gStamp = fn; }
uint32_t CpuMeter::stampCycles() { return gStamp ? gStamp() : 0u; }
CpuMeter::CycleFn CpuMeter::cycleSource() { return gCycles; }
uint32_t CpuMeter::cycles() { return gCycles ? gCycles() : 0u; }

void CpuMeter::addBlock(uint32_t cyc) {
    const uint32_t l = permille(cyc);
    last_ = l;
    // A += (L - A) / 32 in A << 5 units.
    avgQ_ = avgQ_ - (avgQ_ >> 5) + l;
    if (l > winMax_) winMax_ = l;
    if (--winLeft_ == 0) {
        prevMax_ = winMax_;
        winMax_ = 0;
        winLeft_ = kWindowBlocks;
    }
    ++blocks_;
    if (l >= kOverrunPermille) {
        ++overruns_;
        lastOverrun_ = blocks_;
    }
}

void CpuMeter::reset() { *this = CpuMeter{}; }

}  // namespace audio
}  // namespace ot
