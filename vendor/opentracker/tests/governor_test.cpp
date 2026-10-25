// 2.x synth voice governor (src/core/audio/governor.h, spec 2x-02 §3):
// overrun cut, near-overrun risk and average cuts with the floor B - 1 and
// the 86-block hold-off, restore after 690 calm blocks, 0 voices = no-op,
// offline off, reset on Stop / load.
#include "test.h"

#include "audio/cpu_meter.h"
#include "audio/governor.h"

using namespace ot::audio;

namespace {
struct FakePool : IVoicePool {
    int n = 0;
    int stolen = 0;
    int sounding() const override { return n; }
    bool stealOne() override {
        if (n <= 0) return false;
        --n;
        ++stolen;
        return true;
    }
};

void run(Governor& g, int blocks, uint32_t l, uint32_t a, bool overrun = false) {
    for (int i = 0; i < blocks; ++i) g.evaluate(l, a, overrun);
}
}  // namespace

TEST(governor_zero_voices_noop) {
    Governor g;
    FakePool p;
    CHECK_EQ(g.cap(), 4);
    run(g, 10, 500, 400);       // no pool
    g.setPool(&p);
    g.setBudget(4);
    run(g, 2000, 1200, 950, true);
    CHECK_EQ(g.cap(), 4);
    CHECK_EQ(g.cuts(), 0u);
    CHECK_EQ(p.stolen, 0);
    CHECK(!g.cutRecent());
}

TEST(governor_overrun_cut_and_holdoff) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    g.setBudget(4);
    p.n = 4;
    g.evaluate(1000, 300, false);     // a 100 % block: C := sounding - 1
    CHECK_EQ(g.cap(), 3);
    CHECK_EQ(p.n, 3);
    CHECK_EQ(p.stolen, 1);
    CHECK_EQ(g.cuts(), 1u);
    CHECK(g.cutRecent());
    // Hold-off: 85 more overrun blocks cut nothing ...
    run(g, int(Governor::kHoldOffBlocks) - 1, 300, 300, true);
    CHECK_EQ(g.cap(), 3);
    CHECK_EQ(g.cuts(), 1u);
    // ... the 86th after the cut does, below the floor (overruns are audible).
    g.evaluate(300, 300, true);
    CHECK_EQ(g.cap(), 2);
    run(g, int(Governor::kHoldOffBlocks) - 1, 100, 300);
    g.evaluate(100, 300, true);
    CHECK_EQ(g.cap(), 1);
    CHECK_EQ(g.steals(), 3u);
}

// 2.0.0 cut a voice on any block >= 90 %: the device's synth song (average
// 55-63 %, single peaks 83-97 %, no overrun) went down to one voice. Now a
// single 90-97 % block cuts nothing, a near-overrun pattern cuts one voice
// and peaks never take more than one (the floor B - 1).
TEST(governor_peaks_without_overrun) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    g.setBudget(4);
    p.n = 4;
    // The device pattern: one 90..97 % block every ~1.5 s over an average of 60 %.
    for (int k = 0; k < 20; ++k) {
        g.evaluate(k % 2 ? 930 : 967, 600, false);
        run(g, 500, 550, 600);
    }
    CHECK_EQ(g.cap(), 4);
    CHECK_EQ(g.cuts(), 0u);
    // Two blocks >= 95 % within 1 s: one voice.
    g.evaluate(955, 600, false);
    run(g, 300, 550, 600);
    g.evaluate(955, 600, false);
    CHECK_EQ(g.cap(), 3);
    CHECK_EQ(p.n, 3);
    // One block >= 98 % is a risk on its own, but the floor B - 1 holds.
    run(g, 2000, 985, 700);
    run(g, 2000, 990, 790);
    CHECK_EQ(g.cap(), 3);
    CHECK_EQ(g.cuts(), 1u);
    // A single 98 % block cuts at once from the budget.
    Governor h;
    FakePool q;
    h.setPool(&q);
    q.n = 4;
    h.evaluate(980, 500, false);
    CHECK_EQ(h.cap(), 3);
    // Fewer voices than the cap: C := max(F, min(C, sounding) - 1) = 3, nothing stolen.
    Governor k;
    FakePool r;
    k.setPool(&r);
    r.n = 3;
    k.evaluate(985, 500, false);
    CHECK_EQ(k.cap(), 3);
    CHECK_EQ(r.stolen, 0);
}

TEST(governor_average_cut) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    p.n = 2;                          // fewer voices than the cap
    run(g, 100, 600, 790);            // below 80 %: nothing
    CHECK_EQ(g.cap(), 4);
    g.evaluate(600, 800, false);      // A >= 800 -> C := C - 1
    CHECK_EQ(g.cap(), 3);
    CHECK_EQ(p.n, 2);                 // still fits: nothing stolen
    run(g, 2000, 600, 850);           // the floor B - 1 holds below 88 %
    CHECK_EQ(g.cap(), 3);
    // A deep average (>= 88 %) goes below the floor, one voice per hold-off.
    g.evaluate(600, 880, false);
    CHECK_EQ(g.cap(), 2);
    run(g, int(Governor::kHoldOffBlocks) - 1, 600, 900);
    CHECK_EQ(g.cap(), 2);
    g.evaluate(600, 900, false);
    CHECK_EQ(g.cap(), 1);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(p.stolen, 1);
}

TEST(governor_restore_after_690_calm_blocks) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    p.n = 4;
    g.evaluate(1000, 300, false);
    CHECK_EQ(g.cap(), 3);
    // 70 % average: not calm (>= 650), no cut (< 800): nothing moves.
    run(g, 2000, 700, 700);
    CHECK_EQ(g.cap(), 3);
    run(g, int(Governor::kRestoreBlocks) - 1, 300, 600);
    CHECK_EQ(g.cap(), 3);
    g.evaluate(300, 600, false);
    CHECK_EQ(g.cap(), 4);
    // A 90 % block (no cut) still restarts the calm count.
    g.evaluate(1000, 300, false);
    CHECK_EQ(g.cap(), 2);              // 3 sounding -> 2
    run(g, 400, 300, 500);
    g.evaluate(910, 500, false);
    run(g, int(Governor::kRestoreBlocks) - 1, 300, 500);
    CHECK_EQ(g.cap(), 2);
    g.evaluate(300, 500, false);
    CHECK_EQ(g.cap(), 3);
    // So does an average >= 65 %.
    run(g, 400, 300, 500);
    g.evaluate(300, 660, false);
    run(g, int(Governor::kRestoreBlocks) - 1, 300, 500);
    CHECK_EQ(g.cap(), 3);
    g.evaluate(300, 500, false);
    CHECK_EQ(g.cap(), 4);
    run(g, 5000, 300, 500);            // never above the budget
    CHECK_EQ(g.cap(), 4);
}

TEST(governor_offline_and_reset) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    p.n = 4;
    g.setEnabled(false);
    run(g, 100, 1500, 950, true);
    CHECK_EQ(g.cap(), 4);
    CHECK_EQ(p.n, 4);
    g.setEnabled(true);
    g.evaluate(1000, 300, false);
    CHECK_EQ(g.cap(), 3);
    g.reset();                         // Stop / project load
    CHECK_EQ(g.cap(), 4);
    CHECK_EQ(g.cuts(), 1u);            // counters survive
    g.setBudget(2);
    CHECK_EQ(g.budget(), 2);
    CHECK_EQ(g.cap(), 2);
    g.setBudget(99);
    CHECK_EQ(g.budget(), Governor::kMaxBudget);
}

TEST(governor_from_meter) {
    Governor g;
    FakePool p;
    g.setPool(&p);
    p.n = 4;
    CpuMeter m;
    m.addBlock(CpuMeter::kBudgetCycles);   // overrun
    g.evaluate(m);
    CHECK_EQ(g.cap(), 3);
}
