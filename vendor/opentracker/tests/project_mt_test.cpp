// project.mt codec tests (spec 02 §3).
#include "test.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/project_mt.h"

using namespace ot;

static std::vector<uint8_t> loadFixture(const char* name) {
    const char* dir = std::getenv("OT_FIXTURES");
    std::string path = std::string(dir ? dir : "fixtures") + "/" + name;
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

static uint8_t g_buf[mt::kFileSize];
static uint8_t g_buf2[mt::kFileSize];

TEST(default_header_golden) {
    Project p;
    encodeProject(p, g_buf);
    // "MT", type 0, fw 1.9.2.255, structure 17x4, size 0x914, pad 0.
    CHECK_BYTES(g_buf, 'M', 'T', 0x00, 0x00, 1, 9, 2, 255, 17, 17, 17, 17, 0x14, 0x09, 0x00, 0x00);
}

TEST(default_fields_golden) {
    Project p;
    encodeProject(p, g_buf);
    CHECK_EQ(g_buf[0x010], 1);  // playlist slot 0
    CHECK_EQ(g_buf[0x011], 0);
    CHECK_EQ(g_buf[0x10F], 0);  // playlist position
    CHECK_EQ(g_buf[0x114], 1);  // current pattern
    CHECK_BYTES(g_buf + 0x115, 0, 11, 24, 8);
    CHECK_BYTES(g_buf + 0x1C0, 0x00, 0x00, 0x02, 0x43, 0x20, 0x00);  // 130.0f, 32
    CHECK_BYTES(g_buf + 0x142, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0);
    CHECK_BYTES(g_buf + 0x152, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    CHECK_BYTES(g_buf + 0x428, 'T', 'r', 'a', 'c', 'k', ' ', '1', 0);
    CHECK_BYTES(g_buf + 0x428 + 21, 'T', 'r', 'a', 'c', 'k', ' ', '2', 0);
    CHECK_BYTES(g_buf + 0x4FD, 0, 127, 1, 5, 7, 11, 64, 128, 0, 0, 0, 0, 0, 0, 0, 0, 1, 127);
    CHECK_BYTES(g_buf + 0x5FE, 0x20, 0x4E);  // 20000
    CHECK_BYTES(g_buf + 0x603, 'M', 'i', 'd', 'i', ' ', '9', 0, 0, 'M', 'i', 'd', 'i', ' ', '1', '0', 0);
    CHECK_BYTES(g_buf + 0x810, 'N', 'e', 'w', ' ', 'P', 'r', 'o', 'j', 'e', 'c', 't', 0);
    CHECK_EQ(g_buf[0x831], 0);
    CHECK_EQ(rdU32(g_buf + 0x910), crc32(g_buf, 0x910));
}

TEST(round_trip) {
    static Project p, q;
    p.setTempo(97.25f);
    p.setCurrentPattern(17);
    for (int i = 0; i < 20; ++i) p.playlist[i] = uint8_t(i + 3);
    p.playlistPos = 4;
    p.setTrackName(2, "Kick");
    p.setTrackName(12, "Synth");
    p.setName("My Song");
    p.setFolder("Projects");
    p.setU8(0x4DD, 77);
    encodeProject(p, g_buf);
    ProjectLoadInfo info;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q, &info) == FmtStatus::Ok);
    CHECK(!info.sizeMismatch);
    CHECK(!info.upgraded);
    CHECK_EQ(info.structVersion, 17);
    CHECK_EQ(info.storedCrc, info.computedCrc);
    CHECK(std::memcmp(p.values, q.values, sizeof(p.values)) == 0);
    CHECK(std::memcmp(p.playlist, q.playlist, sizeof(p.playlist)) == 0);
    CHECK_EQ(q.playlistPos, 4);
    CHECK_STR(q.name, "My Song");
    CHECK_STR(q.folder, "Projects");
    encodeProject(q, g_buf2);
    CHECK(std::memcmp(g_buf, g_buf2, sizeof(g_buf)) == 0);
}

TEST(bad_crc_accepted) {
    Project p;
    encodeProject(p, g_buf);
    wrU32(g_buf + 0x910, 0);
    Project q;
    q.setTempo(1.0f);
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    CHECK_NEAR(q.tempo(), 130.0, 0);
}

TEST(type_must_be_zero) {
    Project p;
    encodeProject(p, g_buf);
    wrU16(g_buf + 2, 1);  // tracker-lib createProject writes 1 (spec 02 §11)
    Project q;
    q.setName("Untouched");
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::WrongType);
    CHECK_STR(q.name, "Untouched");
}

TEST(size_mismatch_unsaved) {
    Project p;
    p.setName("Named");
    p.setFolder("Projects");
    encodeProject(p, g_buf);
    wrU16(g_buf + 0x0C, 0x900);
    Project q;
    ProjectLoadInfo info;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q, &info) == FmtStatus::Ok);
    CHECK(info.sizeMismatch);
    CHECK_STR(q.name, "Unsaved Project");
    CHECK_STR(q.folder, "");
    CHECK_EQ(q.u8(0x140), 3);
}

TEST(short_file_reads_zero) {
    Project p;
    p.setTempo(150.0f);
    encodeProject(p, g_buf);
    // Truncate before the tempo field: the tail reads 0 (spec 10 §0.2).
    Project q;
    q.setTempo(50.0f);
    CHECK(decodeProject(g_buf, 0x100, q) == FmtStatus::Ok);
    CHECK_NEAR(q.tempo(), 0.0, 0);
    CHECK_STR(q.name, "");  // size field read from file = 0x914, name bytes 0
}

TEST(name_nul_forced) {
    Project p;
    encodeProject(p, g_buf);
    std::memset(g_buf + 0x810, 'A', 0x100);  // fill name + start of folder
    std::memset(g_buf + 0x831, 'B', 0x910 - 0x831);
    Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    CHECK_EQ(std::strlen(q.name), 32u);
    CHECK_EQ(std::strlen(q.folder), 221u);
    CHECK_EQ(q.folder[0], 'B');
    encodeProject(q, g_buf2);
    CHECK_EQ(g_buf2[0x830], 0);
    CHECK_EQ(g_buf2[0x90E], 0);
}

TEST(recorder_source_0x134_keeps_every_tracker_source) {
    // Original Tracker: 0 line in, 1 radio, 2 mic LG, 3 mic HG all survive a
    // load (the Mini rule `> 2 -> 0`, spec 21 §3.2, is not applied here).
    for (uint8_t src = 0; src <= 4; ++src) {
        Project p;
        p.setU8(0x134, src);
        encodeProject(p, g_buf);
        CHECK_EQ(g_buf[0x134], src);
        Project q;
        CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
        CHECK_EQ(q.u8(0x134), src <= 3 ? src : 0);
    }
}

TEST(sanitiser) {
    Project p;
    encodeProject(p, g_buf);
    g_buf[0x110] = 0xF0;        // <0 -> 48
    g_buf[0x111] = 64;          // >63 -> 0
    g_buf[0x113] = 50;          // >49 -> 0
    g_buf[0x12C + 3] = 2;       // >1 -> 0
    g_buf[0x134] = 4;           // >3 -> 0 (1.9.2 rule; recorder source 0..3)
    wrU16(g_buf + 0x142, 0);    // not 1..255 -> 1
    wrU16(g_buf + 0x144, 256);
    wrU16(g_buf + 0x146, 255);  // valid
    wrU16(g_buf + 0x15E, 7);    // [0] forced 0
    wrU16(g_buf + 0x160, uint16_t(int16_t(-255)));  // valid
    wrU16(g_buf + 0x162, uint16_t(int16_t(-256)));  // -> 0
    wrU16(g_buf + 0x164, 300);                      // -> 0
    wrF32(g_buf + 0x1C0, 1000.5f);                  // -> 130
    wrU16(g_buf + 0x1C4, 256);                      // -> 32
    g_buf[0x2F6 + 11] = 4;                          // -> 0
    g_buf[0x2F6 + 10] = 3;                          // valid
    g_buf[0x4FD + 16 * 2] = 17;                     // channel > 16 -> record index 2
    g_buf[0x4FD + 16 * 6] = 16;                     // valid
    g_buf[0x4FD + 16 * 3 + 1] = 0x80;               // velocity bit7 -> 127
    g_buf[0x4FD + 16 * 4 + 2] = 131;                // CC > 130 -> 0
    g_buf[0x4FD + 16 * 4 + 3] = 130;                // valid
    wrU16(g_buf + 0x5FE, 99);                       // -> 20000
    g_buf[0x600] = 2;
    g_buf[0x601] = 5;
    g_buf[0x602] = 2;
    g_buf[0x742 + 7] = 9;
    g_buf[0x80C] = 101;
    Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    CHECK_EQ(q.u8(0x110), 48);
    CHECK_EQ(q.u8(0x111), 0);
    CHECK_EQ(q.u8(0x113), 0);
    CHECK_EQ(q.u8(0x12F), 0);
    CHECK_EQ(q.u8(0x134), 0);
    CHECK_EQ(q.u16(0x142), 1);
    CHECK_EQ(q.u16(0x144), 1);
    CHECK_EQ(q.u16(0x146), 255);
    CHECK_EQ(q.u16(0x15E), 0);
    CHECK_EQ(int16_t(q.u16(0x160)), -255);
    CHECK_EQ(q.u16(0x162), 0);
    CHECK_EQ(q.u16(0x164), 0);
    CHECK_NEAR(q.tempo(), 130.0, 0);
    CHECK_EQ(q.u16(0x1C4), 32);
    CHECK_EQ(q.u8(0x2F6 + 11), 0);
    CHECK_EQ(q.u8(0x2F6 + 10), 3);
    CHECK_EQ(q.midiInstrument(2).channel, 2);
    CHECK_EQ(q.midiInstrument(6).channel, 16);
    CHECK_EQ(q.midiInstrument(3).velocity, 127);
    CHECK_EQ(q.midiInstrument(4).cc[0], 0);
    CHECK_EQ(q.midiInstrument(4).cc[1], 130);
    CHECK_EQ(q.u16(0x5FE), 20000);
    CHECK_EQ(q.u8(0x600), 0);
    CHECK_EQ(q.u8(0x601), 0);
    CHECK_EQ(q.u8(0x602), 0);
    CHECK_EQ(q.u8(0x749), 0);
    CHECK_EQ(q.u8(0x80C), 0);

    // Track-index check (0x152) only with a runtime limit.
    encodeProject(p, g_buf);
    g_buf[0x152] = 40;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    CHECK_EQ(q.u8(0x152), 40);
    CHECK(decodeProject(g_buf, sizeof(g_buf), q, nullptr, 13) == FmtStatus::Ok);
    CHECK_EQ(q.u8(0x152), 1);
}

static int g_upgradeFrom = -1;
static uint32_t g_upgradeLen = 0;
static void upgradeHook(uint8_t* block, uint32_t len, uint8_t from) {
    g_upgradeFrom = from;
    g_upgradeLen = len;
    block[0x114 - 0x10] = 9;  // pretend the upgrade changed the current pattern
}

TEST(upgrade_hook) {
    Project p;
    encodeProject(p, g_buf);
    g_buf[8] = 16;
    setProjectUpgradeHook(&upgradeHook);
    Project q;
    ProjectLoadInfo info;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q, &info) == FmtStatus::Ok);
    CHECK(info.upgraded);
    CHECK_EQ(g_upgradeFrom, 16);
    CHECK_EQ(g_upgradeLen, 0x800u);
    CHECK_EQ(q.currentPattern(), 9);
    g_upgradeFrom = -1;
    g_buf[8] = 17;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q, &info) == FmtStatus::Ok);
    CHECK_EQ(g_upgradeFrom, -1);
    setProjectUpgradeHook(nullptr);
}

TEST(tracker_lib_template) {
    std::vector<uint8_t> tpl = loadFixture("tl_project_template.mt");
    CHECK_EQ(tpl.size(), size_t(mt::kFileSize));
    if (tpl.size() != mt::kFileSize) return;
    static Project q;
    ProjectLoadInfo info;
    CHECK(decodeProject(tpl.data(), tpl.size(), q, &info) == FmtStatus::Ok);
    CHECK_EQ(info.structVersion, 17);
    CHECK_EQ(info.storedCrc, 0u);  // tracker-lib writes 0 (spec 02 §11)
    CHECK_NEAR(q.tempo(), 130.0, 0);
    CHECK_EQ(q.defaultPatternLength(), 32);
    CHECK_EQ(q.currentPattern(), 1);
    CHECK_EQ(q.playlist[0], 1);
    CHECK_STR(q.name, "New Project");
    CHECK_STR(q.folder, "Projects");
    char buf[32];
    q.trackName(0, buf, sizeof(buf));
    CHECK_STR(buf, "Track 1");
    q.trackName(15, buf, sizeof(buf));
    CHECK_STR(buf, "Midi 16");
    CHECK_EQ(q.midiInstrument(15).channel, 15);
    CHECK_NEAR(q.f32(mt::kReverbDiffusion), 0.68, 1e-6);
    // Song + values blocks survive a re-encode byte-for-byte.
    encodeProject(q, g_buf);
    CHECK(std::memcmp(g_buf + 0x10, tpl.data() + 0x10, 0x800) == 0);
    CHECK(std::memcmp(g_buf, tpl.data(), 0x0E) == 0);
}

// ---------------------------------------------------------------------------
// Tracker Mini additions (spec 21 §3): synth-slot records, names and mutes
// of tracks 13..16.

TEST(mini_slot_defaults_match_tracker_lib_template) {
    // tracker-lib's LATEST_PROJECT_TEMPLATE is a Mini project: its slot area
    // holds the three default records (-9 dB, poly 1, WTFM / ACD / VAP, "Init").
    std::vector<uint8_t> tpl = loadFixture("tl_project_template.mt");
    CHECK_EQ(tpl.size(), size_t(mt::kFileSize));
    if (tpl.size() != mt::kFileSize) return;
    uint8_t rec[mt::kSynthSlotSize];
    for (int s = 0; s < kSynthSlotCount; ++s) {
        SynthSlotRecord::defaults(s).encode(rec);
        CHECK(std::memcmp(rec, tpl.data() + mt::synthSlotOff(s), mt::kSynthSlotSize) == 0);
    }
    Project p;   // New Project
    encodeProject(p, g_buf);
    // 2.0 owner decision: a New Project's Synth 1 is ACD (WTFM has no engine
    // until 2.1); everything else in the slot area is the template's.
    SynthSlotRecord s0 = SynthSlotRecord::defaults(0);
    s0.model = uint32_t(SynthModel::ACD);
    s0.encode(rec);
    CHECK(std::memcmp(g_buf + mt::synthSlotOff(0), rec, mt::kSynthSlotSize) == 0);
    CHECK(std::memcmp(g_buf + mt::synthSlotOff(1), tpl.data() + mt::synthSlotOff(1), 0x80C - mt::synthSlotOff(1)) == 0);
    // Names 9..16 as the Mini init (0x603..0x642).
    CHECK(std::memcmp(g_buf + 0x603, tpl.data() + 0x603, 0x40) == 0);
    SynthSlotRecord r = p.synthSlot(0);
    CHECK_NEAR(r.gainDb, -9.0, 0);
    CHECK_EQ(r.polyphony, 1u);
    CHECK_EQ(r.model, uint32_t(SynthModel::ACD));   // 2.1: back to WTFM
    CHECK_STR(r.patchName, "Init");
    CHECK_EQ(SynthSlotRecord::defaults(0).model, uint32_t(SynthModel::WTFM));   // load sanitiser: Mini
    CHECK_EQ(p.synthSlot(1).model, uint32_t(SynthModel::ACD));
    CHECK_EQ(p.synthSlot(2).model, uint32_t(SynthModel::VAP));
    CHECK_EQ(synthEngineId(SynthModel::PERC), 6);
    CHECK_STR(synthModelName(SynthModel::WTFM), "WTFM");
}

TEST(mini_slot_accessors_round_trip) {
    Project p;
    SynthSlotRecord r;
    r.gainDb = -3.5f;
    r.polyphony = 3;
    r.model = uint32_t(SynthModel::PERC);
    r.pan = -25.0f;
    copyStr(r.patchName, sizeof(r.patchName), "Kit 808");
    r.reverbSend = 12.5f;
    r.delaySend = 100.0f;
    const uint8_t mac[6] = {0, 10, 63, 127, 5, 0};
    std::memcpy(r.macros, mac, 6);
    p.setSynthSlot(1, r);
    encodeProject(p, g_buf);
    const uint32_t o = 0x74C + 0x40;
    CHECK_EQ(rdF32(g_buf + o), -3.5f);
    CHECK_EQ(rdU32(g_buf + o + 4), 3u);
    CHECK_EQ(rdU32(g_buf + o + 8), 4u);
    CHECK_EQ(rdF32(g_buf + o + 0xC), -25.0f);
    CHECK_STR(reinterpret_cast<const char*>(g_buf + o + 0x10), "Kit 808");
    CHECK_EQ(rdF32(g_buf + o + 0x30), 12.5f);
    CHECK_EQ(rdF32(g_buf + o + 0x34), 100.0f);
    CHECK_BYTES(g_buf + o + 0x38, 0, 10, 63, 127, 5, 0, 0, 0);
    static Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    encodeProject(q, g_buf2);
    CHECK(std::memcmp(g_buf, g_buf2, mt::kFileSize) == 0);
    SynthSlotRecord b = q.synthSlot(1);
    CHECK(b.valid());
    CHECK_STR(b.patchName, "Kit 808");
    CHECK_EQ(b.macros[3], 127);
    // Out-of-range slot index: not written, reads the default.
    q.setSynthSlot(3, r);
    encodeProject(q, g_buf2);
    CHECK(std::memcmp(g_buf, g_buf2, mt::kFileSize) == 0);
}

// One bad field makes the whole slot revert to its default; the other
// slots and the neighbouring bytes stay.
TEST(mini_slot_sanitiser) {
    struct Bad {
        uint32_t off;
        uint32_t u;
        bool isFloat;
        float f;
    };
    const float nan = std::nanf("");
    const Bad bad[] = {
        {0x00, 0, true, 12.01f},  {0x00, 0, true, -96.5f}, {0x00, 0, true, nan},
        {0x04, 9, false, 0},      {0x08, 5, false, 0},      {0x08, 0xFFFFFFFFu, false, 0},
        {0x0C, 0, true, 100.5f},  {0x0C, 0, true, -101.0f}, {0x30, 0, true, -0.5f},
        {0x30, 0, true, 101.0f},  {0x34, 0, true, nan},     {0x34, 0, true, 100.01f},
        {0x38, 0x80, false, 0},   {0x3D, 0xFF, false, 0},
    };
    for (const Bad& b : bad) {
        for (int s = 0; s < 3; ++s) {
            Project p;
            for (int k = 0; k < 3; ++k) {
                SynthSlotRecord r = SynthSlotRecord::defaults(k);
                r.gainDb = 1.0f;
                r.polyphony = 4;
                r.model = 1;
                copyStr(r.patchName, sizeof(r.patchName), "Mine");
                r.macros[0] = 9;
                p.setSynthSlot(k, r);
            }
            encodeProject(p, g_buf);
            const uint32_t o = 0x74C + 0x40 * uint32_t(s) + b.off;
            if (b.isFloat) wrF32(g_buf + o, b.f);
            else if (b.off >= 0x38) g_buf[o] = uint8_t(b.u);
            else wrU32(g_buf + o, b.u);
            g_buf[0x74C + 0x40 * s + 0x3E] = 0x5A;   // padding: not checked, reset with the slot
            static Project q;
            CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
            uint8_t want[0x40];
            SynthSlotRecord::defaults(s).encode(want);
            CHECK(std::memcmp(q.values + (0x74C + 0x40 * s - 0x110), want, 0x40) == 0);
            for (int k = 0; k < 3; ++k) {
                if (k == s) continue;
                const SynthSlotRecord o2 = q.synthSlot(k);
                CHECK_EQ(o2.polyphony, 4u);
                CHECK_STR(o2.patchName, "Mine");
            }
        }
    }
    // Range ends are valid; the patch name and padding are not checked.
    Project p;
    SynthSlotRecord r;
    r.gainDb = 12.0f;
    r.polyphony = 8;
    r.model = 4;
    r.pan = -100.0f;
    r.reverbSend = 100.0f;
    r.delaySend = 0.0f;
    std::memset(r.macros, 127, 6);
    r.pad[0] = 0x77;
    std::memset(r.patchName, 'z', 32);
    r.patchName[32] = 0;
    CHECK(r.valid());
    p.setSynthSlot(0, r);
    r.gainDb = -96.0f;
    r.polyphony = 0;
    r.model = 0;
    r.pan = 100.0f;
    CHECK(r.valid());
    p.setSynthSlot(2, r);
    encodeProject(p, g_buf);
    static Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    encodeProject(q, g_buf2);
    CHECK(std::memcmp(g_buf, g_buf2, mt::kFileSize) == 0);
    // A 1.9.2 file (slot area zero) passes: three ACD slots, polyphony 0.
    std::memset(g_buf + 0x74C, 0, 0xC0);
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    for (int s = 0; s < 3; ++s) {
        const SynthSlotRecord z = q.synthSlot(s);
        CHECK_EQ(z.polyphony, 0u);
        CHECK_EQ(z.model, 0u);
        CHECK_STR(z.patchName, "");
    }
    // Direct call: reset mask and bipolar macro defaults.
    q.setU32(0x74C + 0x40 + 8, 7);
    const uint8_t bip[3] = {0, 0x05, 0};
    CHECK_EQ(sanitizeSynthSlots(q, bip), 0x02);
    CHECK_BYTES(q.values + (0x74C + 0x40 + 0x38 - 0x110), 63, 0, 63, 0, 0, 0);
    CHECK_EQ(sanitizeSynthSlots(q), 0);
}

TEST(mini_track_names_and_mutes_13_16) {
    Project p;
    char buf[16];
    for (int t = 12; t < 16; ++t) {
        p.trackName(t, buf, sizeof(buf));
        char want[24];
        std::snprintf(want, sizeof(want), "Midi %d", t + 1);
        CHECK_STR(buf, want);
    }
    CHECK(p.setTrackName(12, "Bass"));
    CHECK(p.setTrackName(15, "LongName9"));   // 7 characters kept
    for (int t = 0; t < 16; ++t) CHECK(!p.trackMuted(t));
    CHECK(p.setTrackMuted(12, true));
    CHECK(p.setTrackMuted(15, true));
    CHECK(p.setTrackMuted(8, true));
    CHECK(p.setTrackMuted(2, true));
    CHECK(!p.setTrackMuted(16, true));
    encodeProject(p, g_buf);
    CHECK_STR(reinterpret_cast<const char*>(g_buf + 0x623), "Bass");
    CHECK_STR(reinterpret_cast<const char*>(g_buf + 0x63B), "LongNam");
    CHECK_BYTES(g_buf + 0x742, 1, 0, 0, 0, 1, 0, 0, 1);
    CHECK_EQ(g_buf[0x12E], 1);
    g_buf[0x745] = 3;   // > 1 -> 0 (stock check on all 8 entries)
    static Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    CHECK(q.trackMuted(12) && q.trackMuted(15) && q.trackMuted(8) && q.trackMuted(2));
    CHECK(!q.trackMuted(13) && !q.trackMuted(11));
    q.trackName(12, buf, sizeof(buf));
    CHECK_STR(buf, "Bass");
}

TEST(mini_values_block_raw_round_trip) {
    // Bytes nobody interprets survive decode/encode, including the slot
    // area padding and bytes after a patch name's NUL.
    std::vector<uint8_t> tpl = loadFixture("tl_project_template.mt");
    if (tpl.size() != mt::kFileSize) return;
    std::memcpy(g_buf, tpl.data(), mt::kFileSize);
    g_buf[0x74C + 0x3E] = 0xA5;
    g_buf[0x74C + 0x40 + 0x10 + 20] = 'q';   // after "Init\0"
    g_buf[0x80B] = 0x11;
    for (uint32_t o = 0x643; o < 0x742; ++o) g_buf[o] = uint8_t(o * 7);
    static Project q;
    CHECK(decodeProject(g_buf, sizeof(g_buf), q) == FmtStatus::Ok);
    encodeProject(q, g_buf2);
    CHECK(std::memcmp(g_buf + 0x10, g_buf2 + 0x10, 0x800) == 0);
}
