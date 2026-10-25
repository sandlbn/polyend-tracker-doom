// I2S receive liveness (src/hal/rx_watch.h, spec 22 §7 row 21): a receiver
// that stops completing blocks must not keep feeding the same block to the
// recorder; it is reported stale until the main loop restarts it.
#include "test.h"

#include "hal/rx_watch.h"

using ot::hal::RxWatch;

namespace {

// Fake I2S RX: a two-half ring like audio_sgtl5000.cpp, filled with a block
// number per completed half; halt() stops it (no more blocks, optional FEF).
struct FakeRx {
    static constexpr uint32_t kHalf = 256;   // 128 stereo frames
    int16_t ring[2 * kHalf] = {};
    uint32_t blocks = 0;
    int half = -1;
    bool running = true;
    bool fef = false;

    void block() {   // RX DMA half/major interrupt
        if (!running) return;
        half = half == 0 ? 1 : 0;
        blocks++;
        for (uint32_t i = 0; i < kHalf; ++i) ring[uint32_t(half) * kHalf + i] = int16_t(blocks);
    }
    void restart() {   // what serviceRx() does to the hardware
        running = true;
        fef = false;
        half = -1;
    }
};

// One audio period: RX completes a block, then the TX callback asks for it.
const int16_t* period(FakeRx& rx, RxWatch& w) {
    rx.block();
    return w.pick(rx.blocks, rx.fef, rx.half, rx.ring, FakeRx::kHalf);
}

}  // namespace

TEST(live_receiver_hands_out_each_new_block) {
    FakeRx rx;
    RxWatch w;
    for (int i = 1; i <= 50; ++i) {
        const int16_t* b = period(rx, w);
        CHECK(b != nullptr);
        if (b) CHECK_EQ(b[0], int16_t(i));
    }
    CHECK(!w.needsRecovery());
    CHECK_EQ(w.staleBlocks(), 0u);
}

TEST(halted_receiver_never_returns_the_frozen_block) {
    FakeRx rx;
    RxWatch w;
    for (int i = 0; i < 10; ++i) period(rx, w);
    rx.running = false;   // RX stops advancing; the last half stays in the ring
    int frozen = 0, none = 0;
    for (int i = 0; i < 100; ++i) {
        const int16_t* b = period(rx, w);
        if (b) frozen++;
        else none++;
    }
    // Only the slack blocks may still see the last block; then nothing.
    CHECK(frozen < int(RxWatch::kStallBlocks));
    CHECK(none > 90);
    CHECK(w.needsRecovery());
    CHECK(!w.stallWasFifoError());
    CHECK(w.staleBlocks() > 90u);
    // Main loop restarts the receiver: counted, input live again.
    rx.restart();
    w.recovered(rx.blocks);
    CHECK(!w.needsRecovery());
    CHECK_EQ(w.recoveries(), 1u);
    const int16_t* b = period(rx, w);
    CHECK(b != nullptr);
    if (b) CHECK_EQ(b[0], int16_t(rx.blocks));
    for (int i = 0; i < 20; ++i) CHECK(period(rx, w) != nullptr);
}

TEST(fifo_error_is_a_stall_at_once) {
    FakeRx rx;
    RxWatch w;
    for (int i = 0; i < 5; ++i) period(rx, w);
    rx.fef = true;        // RCSR FEF (bit 18): the receiver halts
    rx.running = false;
    CHECK(period(rx, w) == nullptr);
    CHECK(w.needsRecovery());
    CHECK(w.stallWasFifoError());
    rx.restart();
    w.recovered(rx.blocks);
    CHECK(period(rx, w) != nullptr);
    CHECK_EQ(w.recoveries(), 1u);
}

TEST(receiver_that_never_started_is_a_stall) {
    FakeRx rx;
    rx.running = false;
    RxWatch w;
    for (int i = 0; i < 10; ++i) CHECK(period(rx, w) == nullptr);
    CHECK(w.needsRecovery());
}

TEST(a_late_block_within_the_slack_is_not_a_stall) {
    FakeRx rx;
    RxWatch w;
    for (int i = 0; i < 5; ++i) period(rx, w);
    // ISR jitter: the TX callback runs twice before the next RX block.
    CHECK(w.pick(rx.blocks, false, rx.half, rx.ring, FakeRx::kHalf) != nullptr);
    CHECK(period(rx, w) != nullptr);
    CHECK(!w.needsRecovery());
}
