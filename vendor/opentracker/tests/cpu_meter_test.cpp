// 2.x CPU meter (src/core/audio/cpu_meter.h, spec 2x-02 §1): L / A / P,
// overruns, the injected cycle source and the engine fillCallback hook.
#include "test.h"

#include <memory>
#include <vector>

#include "audio/cpu_meter.h"
#include "audio/engine.h"

using namespace ot;
using namespace ot::audio;

namespace {
constexpr uint32_t kB = CpuMeter::kBudgetCycles;
uint32_t pm(uint32_t permille) { return kB / 1000u * permille + (kB % 1000u) * permille / 1000u + 1; }

// Fake cycle counter: each read advances by the next scripted step.
uint32_t gNow = 0;
uint32_t gStep = 0;
uint32_t gReads = 0;
uint32_t fakeCycles() {
    // Reads alternate begin / end of a block: the end read lands gStep later.
    if (gReads++ & 1) gNow += gStep;
    return gNow;
}
}  // namespace

TEST(cpu_meter_last_avg_peak) {
    CpuMeter m;
    CHECK(!m.hasData());
    CHECK_EQ(m.peak(), 0u);
    m.addBlock(pm(400));
    CHECK(m.hasData());
    CHECK_EQ(m.last(), 400u);
    CHECK_EQ(m.peak(), 400u);
    // A: alpha 1/32 -> after one block 400/32 = 12.
    CHECK_EQ(m.avg(), 12u);
    for (int i = 0; i < 400; ++i) m.addBlock(pm(400));
    CHECK(m.avg() >= 395u && m.avg() <= 400u);
    // ~100 ms time constant: 32 blocks from 400 to 0 leave ~400/e.
    for (int i = 0; i < 32; ++i) m.addBlock(0);
    CHECK(m.avg() >= 130u && m.avg() <= 160u);
    CHECK_EQ(m.overruns(), 0u);
    CHECK(!m.overrunLast());
}

TEST(cpu_meter_peak_window_and_hold) {
    CpuMeter m;
    m.addBlock(pm(800));                       // block 1 of window 1
    CHECK_EQ(m.peak(), 800u);
    for (uint32_t i = 1; i < CpuMeter::kWindowBlocks; ++i) m.addBlock(pm(100));
    // Window 1 closed: its max is held through window 2.
    CHECK_EQ(m.peak(), 800u);
    for (uint32_t i = 0; i < CpuMeter::kWindowBlocks - 1; ++i) m.addBlock(pm(200));
    CHECK_EQ(m.peak(), 800u);
    m.addBlock(pm(200));                       // window 2 closes: hold over
    CHECK_EQ(m.peak(), 200u);
    m.addBlock(pm(300));                       // live window above the held one
    CHECK_EQ(m.peak(), 300u);
}

TEST(cpu_meter_overrun) {
    CpuMeter m;
    m.addBlock(pm(999));
    CHECK_EQ(m.overruns(), 0u);
    m.addBlock(kB);                            // 100 %
    CHECK_EQ(m.last(), 1000u);
    CHECK(m.overrunLast());
    CHECK_EQ(m.overruns(), 1u);
    CHECK(m.overrunRecent());
    m.addBlock(2 * kB);
    CHECK_EQ(m.overruns(), 2u);
    for (uint32_t i = 0; i < CpuMeter::kRecentBlocks - 1; ++i) m.addBlock(0);
    CHECK(m.overrunRecent());
    m.addBlock(0);
    CHECK(!m.overrunRecent());
    m.reset();
    CHECK(!m.hasData());
    CHECK_EQ(m.overruns(), 0u);
}

TEST(cpu_meter_engine_hook) {
    std::vector<int16_t> fx(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool(1u << 16);
    std::unique_ptr<Instrument[]> ins(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng(new AudioEngine());
    CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
    int16_t out[kBlock * 2 * 2];
    // No source (host default): the meter stays empty.
    CHECK(CpuMeter::cycleSource() == nullptr);
    CHECK_EQ(CpuMeter::cycles(), 0u);
    AudioEngine::fillCallback(out, kBlock * 2, eng.get());
    CHECK(!eng->cpu().hasData());
    // Fake source: two blocks per callback, each "costs" 35 %.
    gStep = pm(350);
    CpuMeter::setCycleSource(&fakeCycles);
    CpuMeter::setStampSource(nullptr);   // two reads per block (no profile stamps)
    AudioEngine::fillCallback(out, kBlock * 2, eng.get());
    CHECK_EQ(eng->cpu().blocks(), 2u);
    CHECK_EQ(eng->cpu().last(), 350u);
    CHECK_EQ(eng->cpu().peak(), 350u);
    // The governor ran (no pool: nothing to do, cap stays at the budget).
    CHECK_EQ(eng->governor().cap(), Governor::kDefaultBudget);
    // Offline render: governor off, back on afterwards.
    eng->beginOffline();
    CHECK(!eng->governor().enabled());
    eng->endOffline();
    CHECK(eng->governor().enabled());
    CpuMeter::setCycleSource(nullptr);
}

// Block profile (block_profile.h): phase stamps of a live block, the peak
// block of each 1 s window, the costliest synth voice.
namespace {
uint32_t gTick = 0;
uint32_t tickCycles() { return gTick += 100; }   // every read 100 cycles later
}  // namespace

TEST(cpu_meter_block_profile_window) {
    std::vector<int16_t> fx(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool(1u << 16);
    std::unique_ptr<Instrument[]> ins(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng(new AudioEngine());
    CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
    int16_t out[kBlock * 2];
    BlockProfile p;
    uint32_t vmax = 0;
    CHECK(!eng->profile().read(p, vmax));   // no window yet
    CpuMeter::setCycleSource(&tickCycles);
    for (uint32_t b = 0; b < CpuMeter::kWindowBlocks; ++b) AudioEngine::fillCallback(out, kBlock, eng.get());
    CHECK(eng->profile().read(p, vmax));
    CHECK(p.total > 0u);
    CHECK_EQ(p.total, eng->lastBlockProfile().total);   // idle blocks all cost the same
    uint32_t parts = p.seq + p.smpOn + p.synOn + p.ctrl + p.smp + p.synFw + p.dly + p.rev + p.out;
    for (uint32_t c : p.slot) parts += c;
    CHECK(parts > 0u);
    CHECK(parts <= p.total);
    CHECK_EQ(p.nSmpOn, 0);
    CHECK_EQ(vmax, 0u);
    // Stamps off: the meter still works, the phases read 0.
    CpuMeter::setStampSource(nullptr);
    AudioEngine::fillCallback(out, kBlock, eng.get());
    CHECK_EQ(eng->lastBlockProfile().ctrl, 0u);
    CHECK(eng->lastBlockProfile().total > 0u);
    CpuMeter::setCycleSource(nullptr);
}
