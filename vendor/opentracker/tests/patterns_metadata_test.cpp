// patternsMetadata and recents codec tests (spec 02 §5, §7).
#include "test.h"

#include "formats/bytes.h"
#include "formats/patterns_metadata.h"

using namespace ot;

static uint8_t g_buf[pamd::kFileSize];
static uint8_t g_buf2[pamd::kFileSize];
static PatternNames g_n, g_m;

TEST(header_golden) {
    g_n.clear();
    encodePatternNames(g_n, g_buf);
    CHECK_BYTES(g_buf, 'P', 'A', 'M', 'D', 1, 0, 0, 0, 0x10, 0x32, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(pamd::kHeaderSize + 256 * 50, pamd::kFileSize);
}

TEST(record_layout) {
    g_n.clear();
    g_n.set(1, "Intro");
    g_n.set(255, "012345678901234567890123456789XYZ");  // truncated to 30
    encodePatternNames(g_n, g_buf);
    CHECK_BYTES(g_buf + 0x10 + 50, 'I', 'n', 't', 'r', 'o', 0);
    const uint8_t* r = g_buf + 0x10 + 255 * 50;
    CHECK_EQ(r[29], '9');
    CHECK_EQ(r[30], 0);
    for (int i = 31; i < 50; ++i) CHECK_EQ(r[i], 0);
    CHECK_EQ(0x10u + 255u * 50u + 50u, pamd::kFileSize);
}

TEST(round_trip) {
    g_n.clear();
    for (int i = 1; i < 256; i += 7) {
        char b[40];
        std::snprintf(b, sizeof(b), "Pattern name %d", i);
        g_n.set(i, b);
    }
    encodePatternNames(g_n, g_buf);
    CHECK(decodePatternNames(g_buf, sizeof(g_buf), g_m) == FmtStatus::Ok);
    for (int i = 0; i < 256; ++i) CHECK_STR(g_m.get(i), g_n.get(i));
    encodePatternNames(g_m, g_buf2);
    CHECK(std::memcmp(g_buf, g_buf2, sizeof(g_buf)) == 0);
}

TEST(bad_magic_clears) {
    g_n.clear();
    g_n.set(3, "abc");
    encodePatternNames(g_n, g_buf);
    g_buf[3] = 'X';
    g_m.set(4, "stale");
    CHECK(decodePatternNames(g_buf, sizeof(g_buf), g_m) == FmtStatus::BadMagic);
    CHECK_STR(g_m.get(3), "");
    CHECK_STR(g_m.get(4), "");
}

TEST(name_without_nul_absent) {
    g_n.clear();
    encodePatternNames(g_n, g_buf);
    std::memset(g_buf + 0x10 + 2 * 50, 'A', 31);  // no NUL in the first 31 bytes
    std::memset(g_buf + 0x10 + 3 * 50, 'B', 30);  // NUL at +30: present
    CHECK(decodePatternNames(g_buf, sizeof(g_buf), g_m) == FmtStatus::Ok);
    CHECK_STR(g_m.get(2), "");
    CHECK_EQ(std::strlen(g_m.get(3)), 30u);
}

TEST(short_file) {
    g_n.clear();
    g_n.set(1, "One");
    g_n.set(200, "Two hundred");
    encodePatternNames(g_n, g_buf);
    CHECK(decodePatternNames(g_buf, 0x10 + 2 * 50, g_m) == FmtStatus::Ok);
    CHECK_STR(g_m.get(1), "One");
    CHECK_STR(g_m.get(200), "");
}

TEST(recents_round_trip) {
    static Recents r, s;
    static uint8_t buf[recents::kFileSize];
    CHECK_EQ(recents::kFileSize, 5000u);
    r.touch("Projects/First");
    r.touch("Projects/Second");
    encodeRecents(r, buf);
    CHECK_BYTES(buf, 'P', 'r', 'o', 'j', 'e', 'c', 't', 's', '/', 'S');
    CHECK_BYTES(buf + 500, 'P', 'r', 'o', 'j', 'e', 'c', 't', 's', '/', 'F');
    CHECK_EQ(buf[1000], 0);
    decodeRecents(buf, sizeof(buf), s);
    CHECK_STR(s.paths[0], "Projects/Second");
    CHECK_STR(s.paths[1], "Projects/First");
    // A slot with no terminator is truncated to 499 chars.
    std::memset(buf + 500 * 9, 'x', 500);
    decodeRecents(buf, sizeof(buf), s);
    CHECK_EQ(std::strlen(s.paths[9]), 499u);
    decodeRecents(buf, 512, s);
    CHECK_STR(s.paths[0], "Projects/Second");
    CHECK_STR(s.paths[1], "Projects/Fir");
}
