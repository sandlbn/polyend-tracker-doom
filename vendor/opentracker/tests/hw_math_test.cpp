// Clock/divider arithmetic of the device drivers (src/hal/hw_math.h) against the
// spec 01 register values and the public datasheet limits.
#include "test.h"

#include "hal/board.h"
#include "hal/hw_math.h"
#include "hal/i2c_bus.h"
#include "hal/sdhc.h"

using namespace ot;
using namespace ot::hal::hw;

TEST(i2c_scl_400k_class) {
    // spec 01 §0 step 3: 400 kHz class on the 90 MHz bus; ICR 0x1F = /240.
    CHECK_EQ(i2cSclHz(board::kBusHz, 0x1F), 375000u);
    CHECK(i2cSclHz(board::kBusHz, 0x1F) <= 400000u);
    CHECK_EQ(i2cSclHz(board::kBusHz, 0x40 | 0x1F), 187500u);   // MULT = x2
}

TEST(spi2_ft81x_clocks) {
    // spec 01 §4.1: 4.5 MHz during init (FT81x requires < 11 MHz before the
    // clock is set up), 30 MHz afterwards (FT81x max 30 MHz).
    CHECK_EQ(spiSckHz(board::kBusHz, board::kSpi2CtarSlow), 4500000u);
    CHECK_EQ(spiSckHz(board::kBusHz, board::kSpi2CtarFast), 30000000u);
    CHECK_EQ(spiFrameBits(board::kSpi2CtarSlow), 8u);
    CHECK_EQ(spiFrameBits(board::kSpi2CtarFast), 8u);
    CHECK_EQ(board::kSpi2CtarSlow & 0x06000000u, 0u);   // CPOL = CPHA = 0 (mode 0)
    CHECK_EQ(board::kSpi2CtarFast & 0x06000000u, 0u);
}

TEST(sdhc_clocks) {
    // SD spec: identification <= 400 kHz, default speed <= 25 MHz.
    CHECK_EQ(sdhcClockHz(board::kCoreHz, 0x10, 14), 375000u);
    CHECK(sdhcClockHz(board::kCoreHz, 0x10, 14) <= 400000u);
    CHECK_EQ(sdhcClockHz(board::kCoreHz, 0x01, 3), 22500000u);
    CHECK(sdhcClockHz(board::kCoreHz, 0x01, 3) <= 25000000u);
    // board.h steps: identification, high speed (<= 50 MHz, spec 01 §6 "50 MHz"
    // class; 180/4 = 45 MHz is the fastest legal divider), default, fallback.
    CHECK_EQ(sdhcClockHz(board::kCoreHz, board::kSdIdClkfs, board::kSdIdDvs), 375000u);
    CHECK_EQ(sdhcClockHz(board::kCoreHz, board::kSdHsClkfs, board::kSdHsDvs), 45000000u);
    CHECK_EQ(sdhcClockHz(board::kCoreHz, board::kSdDsClkfs, board::kSdDsDvs), 22500000u);
    CHECK_EQ(sdhcClockHz(board::kCoreHz, board::kSdLoClkfs, board::kSdLoDvs), 11250000u);
    CHECK(sdhcClockHz(board::kCoreHz, 0x01, 0) > 50000000u);   // 90 MHz: next step up is illegal
    CHECK_EQ(board::kSdWml & 0xFFu, 128u);                     // spec 01 §6 watermark
    CHECK_EQ((board::kSdWml >> 16) & 0xFFu, 128u);
}

TEST(sdhc_adma2_descriptors) {
    using namespace ot::hal::sdhc::detail;
    // K66 RM ADMA2 descriptor: valid + transfer, length in bits 31:16, end on the last.
    CHECK_EQ(adma2Attr(512, true), (512u << 16) | 0x23u);
    CHECK_EQ(adma2Attr(32768, false), 0x80000021u);
    CHECK(board::kSdAdmaDescBytes <= 65532u);
    CHECK_EQ(adma2DescCount(512, board::kSdAdmaDescBytes), 1u);
    CHECK_EQ(adma2DescCount(board::kSdAdmaDescs * board::kSdAdmaDescBytes, board::kSdAdmaDescBytes),
             board::kSdAdmaDescs);
    CHECK_EQ(adma2DescCount(board::kSdBounceBytes, board::kSdAdmaDescBytes) <= board::kSdAdmaDescs, true);
}

TEST(sdhc_dma_direct_rules) {
    using namespace ot::hal::sdhc::detail;
    const uint32_t b = board::kSdramBase, sz = board::kSdramSize;
    CHECK(dmaDirectOk(0x20001000u, 4096, false, b, sz));         // SRAM_U
    CHECK(dmaDirectOk(0x1FFF8000u, 4096, false, b, sz));         // SRAM_L
    CHECK(!dmaDirectOk(0x20001001u, 512, false, b, sz));         // unaligned
    CHECK(!dmaDirectOk(0x1FFFFE00u, 1024, false, b, sz));        // crosses SRAM_L/SRAM_U
    CHECK(!dmaDirectOk(0x2002FE00u, 1024, false, b, sz));        // past SRAM_U
    CHECK(!dmaDirectOk(0x70000000u, 512, false, b, sz));         // SDRAM bounces by default
    CHECK(dmaDirectOk(0x70000000u, 512, true, b, sz));
    CHECK(!dmaDirectOk(0x70FFFE00u, 1024, true, b, sz));         // past SDRAM end
    CHECK(inCodeBusRam(0x1FFF0000u) && !inCodeBusRam(0x20000000u));
}

TEST(sdhc_cmd6_status) {
    using namespace ot::hal::sdhc::detail;
    uint8_t st[64] = {};
    CHECK(!cmd6HighSpeedSupported(st));
    st[13] = 0x03;           // group 1 support: functions 0 and 1 (bits 400, 401)
    st[16] = 0x01;           // group 1 result: function 1
    CHECK(cmd6HighSpeedSupported(st));
    CHECK_EQ(cmd6Group1Result(st), 1u);
    st[16] = 0x0F;           // cannot switch
    CHECK_EQ(cmd6Group1Result(st), 15u);
}

TEST(i2c_timeout_ignores_preemption) {
    using ot::hal::i2cWaitExpired;
    // Wall clock alone (an ISR held the CPU for 3 ms while the loop polled a few
    // times) must not expire the wait.
    CHECK(!i2cWaitExpired(3000, 5, board::kI2cTimeoutUs));
    CHECK(!i2cWaitExpired(board::kI2cTimeoutUs, board::kI2cMinPolls, board::kI2cTimeoutUs));
    CHECK(i2cWaitExpired(board::kI2cTimeoutUs + 1, board::kI2cMinPolls, board::kI2cTimeoutUs));
    CHECK(!i2cWaitExpired(10, 1000000, board::kI2cTimeoutUs));   // many polls, little time
    CHECK_EQ(board::kI2cBusyRecoverMs, 10u);                     // spec 01 §7.5
}

TEST(sdram_refresh) {
    // spec 01 §2: 15625 ns row refresh at 90 MHz -> RC 86, CTRL = 0x0256.
    CHECK_EQ(sdramRefreshCount(board::kFlexBusHz, 15625), 86u);
    CHECK_EQ(board::kSdramCtrl & 0x1FFu, 86u);
    CHECK_EQ((board::kSdramCtrl >> 9) & 3u, 1u);
    CHECK(sdramRefreshPeriodNs(board::kFlexBusHz, 86) <= 15625u);
}

TEST(uart4_midi_divisor) {
    // spec 01 §8: UART4 on the 90 MHz bus clock -> SBR 180, BRFA 0.
    uint32_t d = uartDivisor32(board::kBusHz, board::kMidiBaud);
    CHECK_EQ(d, 5760u);
    CHECK_EQ(d >> 5, 180u);
    CHECK_EQ(d & 31u, 0u);
    CHECK_EQ(uartBaudFromDivisor32(board::kBusHz, d), 31250u);
}

TEST(i2s_clocks) {
    // spec 01 §5.2: MCLK = 180 MHz * 16 / 255, BCLK = MCLK / 4 = 64 Fs, Fs = MCLK / 256.
    uint32_t mclk = i2sMclkHz(board::kCoreHz, board::kI2sMdr);
    CHECK_EQ(mclk, 11294117u);
    CHECK_EQ(i2sBclkHz(mclk, board::kI2sTcr2), mclk / 4u);
    CHECK_NEAR(double(mclk) / 256.0, board::kSampleRateMilliHz / 1000.0, 1.0);
}

TEST(ft81x_frame_rate) {
    // spec 01 §4.2: 60 MHz / 2 / 1056 / 525 ~= 54.1 Hz.
    CHECK_EQ(ft81xFrameMilliHz(60000000, 2, 1056, 525), 54112u);
}

TEST(blink_heartbeat_when_ok) {
    CHECK(statusLedOn(0, 0));
    CHECK(statusLedOn(0, 99));
    CHECK(!statusLedOn(0, 100));
    CHECK(!statusLedOn(0, 1999));
    CHECK(statusLedOn(0, 2000));
}

TEST(blink_single_code) {
    // code 2 (SDRAM): on 0-249, off 250-499, on 500-749, off until 2500, repeat.
    uint32_t m = 1u << (kBlinkSdram - 1);
    CHECK_EQ(blinkCycleMs(m), 2500u);
    CHECK(statusLedOn(m, 0));
    CHECK(!statusLedOn(m, 250));
    CHECK(statusLedOn(m, 500));
    CHECK(!statusLedOn(m, 750));
    CHECK(!statusLedOn(m, 1000));
    CHECK(!statusLedOn(m, 2499));
    CHECK(statusLedOn(m, 2500));
    int pulses = 0;
    bool prev = false;
    for (uint32_t t = 0; t < 2500; t++) {
        bool on = statusLedOn(m, t);
        if (on && !prev) pulses++;
        prev = on;
    }
    CHECK_EQ(pulses, 2);
}

TEST(blink_multiple_codes_in_turn) {
    // display (1) + SD (7): 1 pulse, gap, 7 pulses, gap.
    uint32_t m = (1u << (kBlinkDisplay - 1)) | (1u << (kBlinkSd - 1));
    CHECK_EQ(blinkCycleMs(m), 2000u + 5000u);
    int pulses = 0;
    bool prev = false;
    for (uint32_t t = 0; t < 2000; t++) {
        bool on = statusLedOn(m, t);
        if (on && !prev) pulses++;
        prev = on;
    }
    CHECK_EQ(pulses, 1);
    pulses = 0;
    prev = false;
    for (uint32_t t = 2000; t < 7000; t++) {
        bool on = statusLedOn(m, t);
        if (on && !prev) pulses++;
        prev = on;
    }
    CHECK_EQ(pulses, 7);
}
