// Boot diagnostic log ring + fault report formatting (src/hal/diag_log.h). The
// device drain (src/hal/diag.cpp) moves take() output into 64-byte CDC packets;
// drainAll() below does the same with a host string.
#include "test.h"

#include <string>

#include "hal/diag_log.cpp"   // platform independent; not part of libcore

using namespace ot::diag;

namespace {
LogBuf gB;

std::string drainAll(LogBuf& b, size_t packet = 64) {
    std::string out;
    char pk[64];
    for (size_t n; (n = take(b, pk, packet)) != 0;) out.append(pk, n);
    return out;
}
}  // namespace

TEST(fmt_numbers_and_bounds) {
    char b[64];
    Fmt f(b, sizeof b);
    f.str("PC ").hex(0x1234ABCDu).ch(' ').dec(42, 5).ch(' ').dec(0);
    CHECK(std::string(b) == "PC 0x1234ABCD    42 0");
    char t[8];
    Fmt g(t, sizeof t);
    g.str("0123456789");
    CHECK(std::string(t) == "0123456");
    CHECK_EQ(g.len, size_t(7));
    char m[24];
    Fmt h(m, sizeof m);
    h.ms(12345);
    CHECK(std::string(m) == "[   12.345] ");
}

TEST(lines_stream_in_packets) {
    reset(gB);
    line(gB, 5, "hello");
    line(gB, 1500, "stage 2: sdram");
    CHECK(pending(gB));
    std::string s = drainAll(gB);
    CHECK(s == "[    0.005] hello\r\n[    1.500] stage 2: sdram\r\n");
    CHECK(!pending(gB));
    // Nothing sent twice; new lines continue the stream.
    line(gB, 1501, "x");
    CHECK(drainAll(gB) == "[    1.501] x\r\n");
}

TEST(overflow_drops_oldest_and_keeps_boot_copy) {
    reset(gB);
    char text[64];
    for (int i = 0; i < 2000; i++) {   // ~2000 * 30 bytes >> 16 KB ring
        std::snprintf(text, sizeof text, "line %04d padding....", i);
        line(gB, uint32_t(i), text);
    }
    CHECK(gB.dropped > 0);
    std::string s = drainAll(gB);
    CHECK_EQ(s.size(), size_t(kRingSize));
    CHECK(s.find("line 1999") != std::string::npos);
    CHECK(s.find("line 0000") == std::string::npos);
    // The boot copy kept the first 12 KB, replayed on request (host sends a byte).
    CHECK_EQ(gB.bootLen, kBootSize);
    requestReplay(gB);
    std::string r = drainAll(gB);
    CHECK_EQ(r.size(), size_t(kBootSize));
    CHECK(r.rfind("[    0.000] line 0000", 0) == 0);
}

TEST(freeze_boot_and_replay_order) {
    reset(gB);
    line(gB, 1, "boot A");
    gB.bootFrozen = true;
    line(gB, 2, "report B");
    drainAll(gB);
    requestReplay(gB);
    line(gB, 3, "new C");
    // Replay (boot copy) first, then the pending stream.
    CHECK(drainAll(gB) == "[    0.001] boot A\r\n[    0.003] new C\r\n");
}

TEST(tail_lines) {
    reset(gB);
    char text[32];
    for (int i = 0; i < 100; i++) {
        std::snprintf(text, sizeof text, "L%d", i);
        line(gB, 0, text);
    }
    char out[4096];
    size_t n = copyFrom(gB, tailStart(gB, 3), out, sizeof out);
    CHECK(std::string(out, n) == "[    0.000] L97\r\n[    0.000] L98\r\n[    0.000] L99\r\n");
    // Fewer lines than asked: everything.
    reset(gB);
    line(gB, 0, "only");
    n = copyFrom(gB, tailStart(gB, 40), out, sizeof out);
    CHECK(std::string(out, n) == "[    0.000] only\r\n");
    // Small destination: truncated, NUL terminated.
    n = copyFrom(gB, 0, out, 5);
    CHECK_EQ(n, size_t(4));
    CHECK_EQ(out[4], 0);
}

TEST(extra_block_goes_first) {
    reset(gB);
    line(gB, 0, "log");
    static const char kFault[] = "FAULT\r\n";
    gB.extra = kFault;
    gB.extraLen = sizeof kFault - 1;
    gB.extraPos = 0;
    CHECK(drainAll(gB, 3) == "FAULT\r\n[    0.000] log\r\n");
}

TEST(fault_report_format) {
    FaultInfo f = {};
    f.ipsr = 3;
    f.excReturn = 0xFFFFFFF9u;
    f.sp = 0x2002FF00u;
    f.frameValid = true;
    f.pc = 0x0000ABCDu;
    f.lr = 0x00001235u;
    f.xpsr = 0x21000000u;
    f.cfsr = (1u << 9) | (1u << 15);   // PRECISERR + BFARVALID (e.g. a gated peripheral)
    f.hfsr = 1u << 30;                 // FORCED
    f.bfar = 0x40066002u;
    f.stage = 4;
    f.stageName = "step 3: I2C0 bus recovery";
    f.ms = 812;
    char out[1024];
    size_t n = formatFault(out, sizeof out, f);
    std::string s(out, n);
    CHECK(s.find("!!! FAULT HardFault (IPSR 3) at boot stage 4 'step 3: I2C0 bus recovery' t=812 ms") == 0);
    CHECK(s.find("PC 0x0000ABCD  LR 0x00001235  xPSR 0x21000000") != std::string::npos);
    CHECK(s.find("SP 0x2002FF00 (MSP)") != std::string::npos);
    CHECK(s.find("BFAR 0x40066002") != std::string::npos);
    CHECK(s.find("decoded: PRECISERR BFARVALID FORCED\r\n") != std::string::npos);
    // Unreadable frame, PSP, tiny buffer: no overrun.
    f.frameValid = false;
    f.excReturn = 0xFFFFFFFDu;
    n = formatFault(out, sizeof out, f);
    s.assign(out, n);
    CHECK(s.find("frame not readable") != std::string::npos);
    CHECK(s.find("(PSP)") != std::string::npos);
    char tiny[16];
    CHECK_EQ(formatFault(tiny, sizeof tiny, f), size_t(15));
    CHECK(std::string(exceptionName(5)) == "BusFault");
}
