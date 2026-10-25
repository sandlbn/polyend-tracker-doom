// CRC-32/ISO-HDLC tests (spec 02 §2).
#include "test.h"

#include <initializer_list>

#include "formats/crc32.h"

using namespace ot;

static uint32_t crcStr(const char* s) { return crc32(reinterpret_cast<const uint8_t*>(s), std::strlen(s)); }

TEST(check_value) { CHECK_EQ(crcStr("123456789"), 0xCBF43926u); }

TEST(known_vectors) {
    CHECK_EQ(crcStr(""), 0x00000000u);
    CHECK_EQ(crcStr("a"), 0xE8B7BE43u);
    CHECK_EQ(crcStr("abc"), 0x352441C2u);
    CHECK_EQ(crcStr("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
    uint8_t zeros[32] = {};
    CHECK_EQ(crc32(zeros, 32), 0x190A55ADu);
    uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(crc32(ff, 4), 0xFFFFFFFFu);
}

TEST(incremental_matches_oneshot) {
    uint8_t buf[1000];
    for (int i = 0; i < 1000; ++i) buf[i] = uint8_t(i * 7 + 3);
    uint32_t one = crc32(buf, sizeof(buf));
    for (size_t split : {size_t(0), size_t(1), size_t(499), size_t(1000)}) {
        uint32_t st = crc32Init();
        st = crc32Update(st, buf, split);
        st = crc32Update(st, buf + split, sizeof(buf) - split);
        CHECK_EQ(crc32Final(st), one);
    }
}

static int g_hookCalls = 0;
static uint32_t fakeHw(const uint8_t* d, size_t n) {
    ++g_hookCalls;
    return crc32Software(d, n);
}

TEST(hardware_hook) {
    setCrc32Hook(&fakeHw);
    CHECK_EQ(crcStr("123456789"), 0xCBF43926u);
    CHECK_EQ(g_hookCalls, 1);
    setCrc32Hook(nullptr);
    CHECK_EQ(crcStr("123456789"), 0xCBF43926u);
    CHECK_EQ(g_hookCalls, 1);
}
