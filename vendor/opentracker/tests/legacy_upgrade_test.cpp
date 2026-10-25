// Synthetic tests for the spec 10 legacy upgrades (formats/legacy_upgrade).
#include <cstring>
#include <vector>

#include "formats/bytes.h"
#include "formats/instrument_pti.h"
#include "formats/legacy_upgrade.h"
#include "formats/pattern_mtp.h"
#include "formats/project_mt.h"
#include "formats/wav.h"
#include "model/fx.h"
#include "test.h"

using namespace ot;
using namespace ot::legacy;

// --- spec 10 §0.4 spot values ---------------------------------------------

TEST(gain_curve_spot_values) {
    CHECK_EQ(gainG2(50), 30);
    CHECK_EQ(gainG2(100), 51);
    CHECK_EQ(gainG4(50), 50);
    CHECK_EQ(gainG4(30), 10);
    for (int v = 1; v <= 26; ++v) CHECK_EQ(gainG4(uint8_t(v)), 1);
    for (int v = 75; v <= 255; ++v) CHECK_EQ(gainG4(uint8_t(v)), 100);
    CHECK_EQ(gainG1(100), 86);
    CHECK_EQ(gainG1(1), 39);
    CHECK_EQ(gainG3(86), 86);
    for (int v = 1; v <= 43; ++v) CHECK_EQ(gainG3(uint8_t(v)), 1);
    for (int v = 93; v <= 255; ++v) CHECK_EQ(gainG3(uint8_t(v)), 100);
    CHECK_EQ(gainG1(0), 0);
    CHECK_EQ(gainG2(0), 0);
    CHECK_EQ(gainG3(0), 0);
    CHECK_EQ(gainG4(0), 0);
    // spec 10 §1.4: 100 -> G1 -> 86 -> G3 -> 86.
    CHECK_EQ(gainG3(gainG1(100)), 86);
    // spec 10 §5.3: default 50 -> G2 30 -> G4 10; v6 51 -> 52, 19 -> 1.
    CHECK_EQ(gainG4(gainG2(50)), 10);
    CHECK_EQ(gainG4(51), 52);
    CHECK_EQ(gainG4(19), 1);
    // Monotonic non-decreasing over 1..255.
    for (int v = 2; v <= 255; ++v) {
        CHECK(gainG1(uint8_t(v)) >= gainG1(uint8_t(v - 1)));
        CHECK(gainG2(uint8_t(v)) >= gainG2(uint8_t(v - 1)));
    }
}

TEST(log_helper_spot_values) {
    CHECK_EQ(logLG(0), 0);
    CHECK_EQ(logLG(16384), 30050);
    CHECK_EQ(logLG(32767), 32767);
    CHECK_EQ(logLG(990), 19049);
    CHECK_EQ(logLG(1), 0);   // far below -36.3 dB: negative saturates to 0
}

// --- spec 10 §1.2 project steps --------------------------------------------

static uint8_t g_img[mt::kFileSize];
static uint8_t g_ref[mt::kFileSize];

static void fillPattern(uint8_t* p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i) {
        seed = seed * 1103515245u + 12345u;
        p[i] = uint8_t(seed >> 16);
    }
}

// Every byte outside `lo..hi` (inclusive ranges) must be unchanged.
static bool sameExcept(const uint8_t* a, const uint8_t* b, std::initializer_list<std::pair<uint32_t, uint32_t>> rs) {
    for (uint32_t i = 0; i < mt::kFileSize; ++i) {
        bool in = false;
        for (auto& r : rs) in = in || (i >= r.first && i <= r.second);
        if (!in && a[i] != b[i]) {
            std::printf("       differs at 0x%x\n", i);
            return false;
        }
    }
    return true;
}

static void stepOnce(uint8_t from, uint32_t c = 0) {
    std::memcpy(g_ref, g_img, sizeof(g_img));
    projectStep(g_img, from, c);
}

TEST(project_step_1_inserts_delay) {
    fillPattern(g_img, mt::kFileSize, 1);
    uint32_t c = 0;
    std::memcpy(g_ref, g_img, sizeof(g_img));
    projectStep(g_img, 1, c);
    CHECK_EQ(c, 4u);
    CHECK(std::memcmp(g_img + 0x120, g_ref + 0x11C, 0x6EF) == 0);
    CHECK_EQ(g_img[0x11A], 50);
    CHECK_EQ(rdU16(g_img + 0x11C), 500);
    CHECK_EQ(g_img[0x11E], 0);
    CHECK_EQ(g_img[0x11F], g_ref[0x11F]);
    CHECK(sameExcept(g_img, g_ref, {{0x11A, 0x11A}, {0x11C, 0x80E}}));
}

TEST(project_steps_2_to_5) {
    fillPattern(g_img, mt::kFileSize, 2);
    stepOnce(2);
    for (uint32_t o : {0x418u, 0x41Cu, 0x420u}) CHECK_EQ(rdU32(g_img + o), 0x3F000000u);
    CHECK_EQ(rdU32(g_img + 0x424), 0x3F2E147Bu);
    CHECK(sameExcept(g_img, g_ref, {{0x418, 0x427}}));

    stepOnce(3);
    for (int i = 1; i <= 8; ++i) {
        const uint8_t* s = g_img + 0x428 + 21 * (i - 1);
        char want[9] = "Track 0";
        want[6] = char('0' + i);
        CHECK(std::memcmp(s, want, 8) == 0);
        CHECK_EQ(s[8], g_ref[0x428 + 21 * (i - 1) + 8]);   // slot rest kept
    }

    stepOnce(4);
    for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(g_img[0x4D0 + i], 100);
    CHECK(sameExcept(g_img, g_ref, {{0x4D0, 0x4D9}}));

    stepOnce(5);
    CHECK_EQ(rdU16(g_img + 0x4DA), 0);
    CHECK_EQ(g_img[0x4DC], 0);
    CHECK_EQ(g_img[0x4DD], 100);
    CHECK(sameExcept(g_img, g_ref, {{0x4DA, 0x4DD}}));
}

TEST(project_steps_6_7_8_9) {
    fillPattern(g_img, mt::kFileSize, 3);
    stepOnce(6);
    for (uint32_t o : {0x4D0u, 0x4D3u, 0x4D7u, 0x4D8u, 0x4D9u, 0x4DDu}) CHECK_EQ(g_img[o], gainG1(g_ref[o]));
    CHECK_EQ(g_img[0x4DA], g_ref[0x4DA]);
    CHECK(sameExcept(g_img, g_ref, {{0x4D0, 0x4D9}, {0x4DD, 0x4DD}}));

    wrU16(g_img + 0x128, 16384);
    stepOnce(7);
    CHECK_EQ(rdU16(g_img + 0x128), 30050);
    for (uint32_t i = 0x4E0; i <= 0x4F5; ++i) CHECK_EQ(g_img[i], 0);
    CHECK(sameExcept(g_img, g_ref, {{0x128, 0x129}, {0x4E0, 0x4F5}}));

    stepOnce(8);
    CHECK_EQ(g_img[0x30A], 1);
    CHECK_EQ(g_img[0x309], 86);
    CHECK(sameExcept(g_img, g_ref, {{0x309, 0x30A}}));

    stepOnce(9);
    CHECK_EQ(g_img[0x115], 0);
    CHECK_EQ(g_img[0x116], 11);
    CHECK_EQ(g_img[0x117], 24);
    CHECK_EQ(rdU32(g_img + 0x4F8), 0x103u);
    CHECK_EQ(g_img[0x4FC], 1);
    CHECK(sameExcept(g_img, g_ref, {{0x115, 0x117}, {0x4F8, 0x4FC}}));
}

TEST(project_step_10_midi_records) {
    fillPattern(g_img, mt::kFileSize, 4);
    stepOnce(10);
    for (int i = 0; i < 16; ++i) {
        const uint8_t* r = g_img + 0x4FD + 16 * i;
        CHECK_EQ(r[0], i);
        CHECK_EQ(r[1], g_ref[0x309 + i]);
        const uint8_t tail[6] = {1, 5, 7, 11, 64, 128};
        CHECK(std::memcmp(r + 2, tail, 6) == 0);
        for (int k = 8; k < 16; ++k) CHECK_EQ(r[k], 0);
    }
    CHECK(sameExcept(g_img, g_ref, {{0x4FD, 0x5FC}}));
}

TEST(project_steps_11_to_14) {
    fillPattern(g_img, mt::kFileSize, 5);
    stepOnce(11);
    for (uint32_t o : {0x4D0u, 0x4D7u, 0x4D8u, 0x4D9u, 0x4DDu, 0x309u}) CHECK_EQ(g_img[o], gainG3(g_ref[o]));
    CHECK(sameExcept(g_img, g_ref, {{0x4D0, 0x4D9}, {0x4DD, 0x4DD}, {0x309, 0x309}}));

    g_img[0x30B] = 101;
    g_img[0x30C] = 7;
    g_img[0x30D] = 100;
    g_img[0x30E] = 9;
    g_img[0x4F6] = 0x41;
    g_img[0x4F7] = 0x42;
    stepOnce(12);
    CHECK_EQ(g_img[0x30B], 0);
    CHECK_EQ(g_img[0x30C], 0);
    CHECK_EQ(g_img[0x30D], 100);
    CHECK_EQ(g_img[0x30E], 9);
    CHECK_EQ(g_img[0x30A], 0x41);
    CHECK_EQ(g_img[0x309], 0x42);
    CHECK_EQ(rdU16(g_img + 0x4F6), 0);
    CHECK(sameExcept(g_img, g_ref, {{0x309, 0x30E}, {0x4F6, 0x4F7}}));

    stepOnce(13);
    CHECK_EQ(rdU16(g_img + 0x5FE), 20000);
    CHECK(sameExcept(g_img, g_ref, {{0x5FE, 0x5FF}}));

    stepOnce(14);
    CHECK_EQ(rdU16(g_img + 0x600), 0);
    CHECK_EQ(g_img[0x602], 0);
    CHECK(sameExcept(g_img, g_ref, {{0x600, 0x602}}));
}

TEST(project_step_15_midi_names) {
    fillPattern(g_img, mt::kFileSize, 6);
    std::memcpy(g_img + 0x609 - 0x10, "ABC", 4);   // source of step (c) with C = 0x10
    stepOnce(15, 0x10);
    const char* names[8] = {"Midi 9", "Midi 10", "Midi 11", "Midi 12", "Midi 13", "Midi 14", "Midi 15", "Midi 16"};
    for (int k = 0; k < 8; ++k) CHECK_STR(reinterpret_cast<const char*>(g_img + 0x603 + 8 * k), names[k]);
    CHECK_EQ(g_img[0x60A], g_ref[0x60A]);   // after "Midi 9" NUL
    for (uint32_t i = 0x643; i <= 0x749; ++i) CHECK_EQ(g_img[i], 0);
    CHECK_STR(reinterpret_cast<const char*>(g_img + 0x810), "ABC");
    CHECK(sameExcept(g_img, g_ref, {{0x603, 0x749}, {0x810, 0x813}}));
    uint32_t c = 0x10;
    std::memcpy(g_ref, g_img, sizeof(g_img));
    projectStep(g_img, 15, c);
    CHECK_EQ(c, 0x10u + 0x207u);
}

TEST(project_step_16) {
    fillPattern(g_img, mt::kFileSize, 7);
    stepOnce(16);
    CHECK_EQ(g_img[0x80C], 0);
    CHECK(sameExcept(g_img, g_ref, {{0x80C, 0x80C}}));
}

TEST(project_chain_counter_and_reject_v0) {
    fillPattern(g_img, mt::kFileSize, 8);
    std::memcpy(g_ref, g_img, sizeof(g_img));
    CHECK(!upgradeProjectImage(g_img, 0));
    CHECK(std::memcmp(g_img, g_ref, sizeof(g_img)) == 0);
    CHECK(upgradeProjectImage(g_img, 17));
    CHECK(std::memcmp(g_img, g_ref, sizeof(g_img)) == 0);
    // The counter reaches step 15 as the sum of steps v..14.
    uint32_t c = 0;
    for (uint8_t v = 1; v < 15; ++v) projectStep(g_img, v, c);
    CHECK_EQ(c, 4u + 16 + 168 + 10 + 4 + 0 + 22 + 2 + 5 + 16 + 0 + 0 + 2 + 3);
}

// A synthetic v11 file shaped like a 1.3.x save (1572 bytes, name at 0x600).
TEST(project_decode_v11_file) {
    static uint8_t file[0x624];
    std::memset(file, 0, sizeof(file));
    file[0] = 'M';
    file[1] = 'T';
    file[4] = 1;
    file[5] = 3;
    file[6] = 1;
    file[7] = 255;
    std::memset(file + 8, 11, 4);
    wrU16(file + 0x0C, 0x624);
    file[0x10] = 1;                                  // playlist
    file[0x114] = 1;
    wrF32(file + 0x1C0, 120.0f);
    wrU16(file + 0x1C4, 63);
    for (uint32_t i = 0; i < 10; ++i) file[0x4D0 + i] = 86;
    file[0x4DD] = 86;
    file[0x309] = 86;
    std::memcpy(file + 0x600, "my old project name 1234", 25);   // 24 chars + NUL
    file[0x4F6] = 3;
    file[0x4F7] = 70;
    wrU32(file + 0x620, 0xDEADBEEF);
    static Project q;
    ProjectLoadInfo info;
    CHECK(decodeProject(file, sizeof(file), q, &info) == FmtStatus::Ok);
    CHECK(info.upgraded);
    CHECK(info.sizeMismatch);
    CHECK_EQ(info.structVersion, 11);
    CHECK_STR(q.name, "Unsaved Project");
    CHECK_EQ(q.u8(0x140), 3);
    CHECK_EQ(q.u8(0x4D0), 86);      // G3(86)
    CHECK_EQ(q.u8(0x30A), 3);       // old 0x4F6
    CHECK_EQ(q.u8(0x309), 70);      // old 0x4F7
    CHECK_EQ(q.u16(0x4F6), 0);
    CHECK_EQ(q.u16(0x5FE), 20000);
    CHECK_EQ(q.u8(0x600), 0);
    CHECK_EQ(q.u8(0x60A), file[0x60A]);   // only survivor of the old name
    char buf[32];
    q.trackName(8, buf, sizeof(buf));
    CHECK_STR(buf, "Midi 9");
    q.trackName(13, buf, sizeof(buf));
    CHECK_STR(buf, "Midi 14");      // old CRC overwritten
    CHECK_NEAR(q.tempo(), 120.0, 0);
}

TEST(project_decode_v2_file_levels_and_names) {
    static uint8_t file[0x43C];
    std::memset(file, 0, sizeof(file));
    file[0] = 'M';
    file[1] = 'T';
    std::memset(file + 8, 2, 4);
    wrU16(file + 0x0C, 0x43C);
    wrU16(file + 0x128, 16384);
    static Project q;
    ProjectLoadInfo info;
    CHECK(decodeProject(file, sizeof(file), q, &info) == FmtStatus::Ok);
    for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(q.u8(0x4D0 + i), 86);
    CHECK_EQ(q.u8(0x4DD), 86);
    CHECK_EQ(q.u16(0x128), 30050);
    CHECK_EQ(q.u8(0x116), 11);
    CHECK_EQ(q.u8(0x117), 24);
    char buf[32];
    q.trackName(0, buf, sizeof(buf));
    CHECK_STR(buf, "Track 1");
    q.trackName(15, buf, sizeof(buf));
    CHECK_STR(buf, "Midi 16");
    MidiInstrument m0 = q.midiInstrument(0), m1 = q.midiInstrument(1);
    CHECK_EQ(m0.velocity, 86);   // step 8 0x309 = 86 -> record 0
    CHECK_EQ(m1.velocity, 1);    // step 8 0x30A = 1 -> record 1
}

TEST(project_decode_rejects_v0) {
    static uint8_t file[0x43C] = {'M', 'T'};
    static Project q;
    CHECK(decodeProject(file, sizeof(file), q) == FmtStatus::Unsupported);
}

// --- spec 10 §2.2 pattern ---------------------------------------------------

static Pattern g_p, g_q;
static uint8_t g_pbuf[mtp::kFileSize];

static void makeLegacyPattern(uint8_t version) {
    encodePattern(g_p, g_pbuf);
    std::memset(g_pbuf + 8, version, 4);
    wrU16(g_pbuf + 0x0C, uint16_t(mtp::kLegacyFileSize));
}

TEST(pattern_v1_curves_and_scan_rule) {
    g_p.setDefault();
    g_p.tracks[0].lastStep = 3;
    g_p.tracks[0].steps[0] = Step{40, 3, {{18, 50}, {18, 100}}};
    g_p.tracks[0].steps[3] = Step{40, 3, {{18, 50}, {0, 0}}};
    g_p.tracks[0].steps[4] = Step{40, 3, {{18, 50}, {0, 0}}};    // beyond lastStep
    g_p.tracks[1].lastStep = 0xFF;                                // skipped track
    g_p.tracks[1].steps[0] = Step{40, 3, {{18, 50}, {0, 0}}};
    g_p.tracks[2].steps[0] = Step{40, 48, {{18, 50}, {0, 0}}};   // MIDI instrument
    g_p.tracks[2].steps[1] = Step{kNoteEmpty, 0, {{18, 50}, {0, 0}}};   // empty note still scanned
    makeLegacyPattern(1);
    PatternLoadInfo info;
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, nullptr, &info);
    CHECK(info.upgraded);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, gainG4(gainG2(50)));
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, 10);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[1].value, gainG4(gainG2(100)));
    CHECK_EQ(g_q.tracks[0].steps[3].fx[0].value, 10);
    CHECK_EQ(g_q.tracks[0].steps[4].fx[0].value, 50);
    CHECK_EQ(g_q.tracks[1].steps[0].fx[0].value, 50);
    CHECK_EQ(g_q.tracks[2].steps[0].fx[0].value, 50);
    CHECK_EQ(g_q.tracks[2].steps[1].fx[0].value, 10);
    for (int t = 8; t < 12; ++t) {
        CHECK_EQ(g_q.tracks[t].lastStep, 3);
        for (auto& s : g_q.tracks[t].steps) CHECK(s.note == kNoteEmpty && s.instrument == 0);
    }
    // v3: only G4.
    makeLegacyPattern(3);
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, nullptr, &info);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, 50);
    CHECK_EQ(g_q.tracks[0].steps[3].fx[0].value, gainG4(50));
    // v4: no curve, tracks still expanded.
    makeLegacyPattern(4);
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, nullptr, &info);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, 50);
    CHECK(info.upperTracksReset);
}

TEST(pattern_v2_beat_slice_transpose) {
    g_p.setDefault();
    g_p.tracks[0].lastStep = 7;
    g_p.tracks[0].steps[0] = Step{40, 5, {}};
    g_p.tracks[0].steps[1] = Step{5, 5, {}};
    g_p.tracks[0].steps[2] = Step{40, 6, {}};       // not beat slice
    g_p.tracks[0].steps[3] = Step{kNoteOff, 5, {}};
    makeLegacyPattern(2);
    uint8_t modes[kSampleInstrumentCount] = {};
    modes[5] = uint8_t(PlayMode::BeatSlice);
    PatternUpgradeContext ctx;
    ctx.instrumentPlayModes = modes;
    ctx.beatSliceReference = 24;   // d = -12
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, &ctx, nullptr);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 28);
    CHECK_EQ(g_q.tracks[0].steps[1].note, 0);    // clamped at 0
    CHECK_EQ(g_q.tracks[0].steps[2].note, 40);
    CHECK_EQ(g_q.tracks[0].steps[3].note, kNoteOff);
    // v3 file: no transpose.
    makeLegacyPattern(3);
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, &ctx, nullptr);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 40);
}

TEST(pattern_legacy_crc_bytes_overwritten) {
    // The old CRC of a 6184-byte file lands on track 8's header / step 0.
    g_p.setDefault();
    g_p.tracks[0].lastStep = 15;
    makeLegacyPattern(3);
    wrU32(g_pbuf + 0x1824, 0x11223344);
    std::memset(g_pbuf + 0x1828, 0, mtp::kFileSize - 0x1828);
    decodePattern(g_pbuf, mtp::kLegacyFileSize, g_q, nullptr, nullptr);
    CHECK_EQ(g_q.tracks[8].lastStep, 15);
    CHECK_EQ(g_q.tracks[8].steps[0].note, kNoteEmpty);
    CHECK_EQ(g_q.tracks[8].steps[0].instrument, 0);
    CHECK_EQ(g_q.tracks[8].steps[0].fx[1].type, 0);
    CHECK_EQ(g_q.tracks[8].steps[0].fx[1].value, 0);
}

// --- spec 10 §3.2 .pti chain -------------------------------------------------

static uint8_t g_h[pti::kHeaderBlockSize];

TEST(pti_steps) {
    fillPattern(g_h, sizeof(g_h), 9);
    g_h[0x4C] = 5;
    ptiStep(g_h, 1);
    CHECK_EQ(g_h[0x4C], 6);
    g_h[0x4C] = 4;
    ptiStep(g_h, 1);
    CHECK_EQ(g_h[0x4C], 4);
    ptiStep(g_h, 2);
    CHECK_EQ(rdU32(g_h + 0xC0), 0x3F800000u);
    CHECK_EQ(rdU16(g_h + 0xC4), 0);
    CHECK_EQ(rdU16(g_h + 0xC6), 3000);
    CHECK_EQ(rdU16(g_h + 0xC8), 0);
    CHECK_EQ(rdU16(g_h + 0xCA), 0);
    CHECK_EQ(rdU32(g_h + 0xCC), 0x3F800000u);
    CHECK_EQ(rdU16(g_h + 0xD0), 1000);
    CHECK_EQ(g_h[0xD2], 0);
    CHECK_EQ(g_h[0xD3], 0);
    ptiStep(g_h, 3);
    CHECK_EQ(g_h[0x180], 0);
    g_h[0x110] = 50;
    ptiStep(g_h, 4);
    CHECK_EQ(g_h[0x110], 30);
    ptiStep(g_h, 5);
    CHECK_EQ(g_h[0x181], 0);
    ptiStep(g_h, 6);
    CHECK_EQ(g_h[0x182], 16);
    ptiStep(g_h, 7);
    CHECK_EQ(g_h[0x183], 0);
    ptiStep(g_h, 8);
    CHECK_EQ(g_h[0x110], 10);
}

static size_t makeMti(uint8_t* out, uint8_t version, uint16_t bodySize, const Instrument& ins) {
    encodePtiHeader(ins, out);
    std::memset(out + 8, version, 4);
    wrU16(out + 0x0C, bodySize);
    return size_t(bodySize) + 0x14;
}

TEST(pti_decode_v1_mti) {
    Instrument a;
    std::strcpy(a.name, "kick.wav");
    a.playMode = 5;
    a.volume = 50;
    a.reverbSend = 77;
    a.env[5].attackMs = 9;
    uint8_t file[0x188];
    size_t len = makeMti(file, 1, 368, a);
    CHECK_EQ(len, 0x184u);
    wrU32(file + 0x180, 0xAABBCCDD);   // old CRC position
    Instrument b;
    PtiLoadInfo info;
    CHECK(decodePtiHeader(file, len, b, &info) == FmtStatus::Ok);
    CHECK_EQ(info.dataOffset, 0x184u);
    CHECK(info.upgraded);
    CHECK_STR(b.name, "kick");
    CHECK_EQ(b.playMode, 6);
    CHECK_EQ(b.env[5].attackMs, 3000);
    CHECK_EQ(b.volume, 10);
    CHECK_EQ(b.reverbSend, 0);
    CHECK_EQ(b.overdrive, 0);
    CHECK_EQ(b.bitDepth, 16);
    CHECK_EQ(b.reserved173, 0);
    CHECK(ptiSlotAccepted(len, info.dataOffset));
    CHECK(ptiNeedsWav(len, info.dataOffset));
    // v4: reverb kept; v8: only G4.
    len = makeMti(file, 4, 372, a);
    CHECK(decodePtiHeader(file, len, b, &info) == FmtStatus::Ok);
    CHECK_EQ(b.reverbSend, 77);
    CHECK_EQ(b.playMode, 5);
    CHECK_EQ(b.env[5].attackMs, 9);
    CHECK_EQ(b.volume, 10);
    a.volume = 19;
    a.bitDepth = 8;
    len = makeMti(file, 6, 372, a);
    CHECK(decodePtiHeader(file, len, b, &info) == FmtStatus::Ok);
    CHECK_EQ(b.volume, 1);
    CHECK_EQ(b.bitDepth, 16);
    a.volume = 50;
    len = makeMti(file, 8, 372, a);
    CHECK(decodePtiHeader(file, len, b, &info) == FmtStatus::Ok);
    CHECK_EQ(b.volume, 50);
    CHECK_EQ(b.bitDepth, 8);
    // Version 0 treated as 1.
    uint8_t raw[pti::kHeaderBlockSize];
    uint8_t v = 0xEE;
    uint32_t off = 0;
    a.playMode = 6;
    len = makeMti(file, 0, 368, a);
    CHECK(readLegacyPtiHeader(file, len, raw, &v, &off) == FmtStatus::Ok);
    CHECK_EQ(v, 0);
    CHECK_EQ(off, 0x184u);
    CHECK_EQ(raw[0x4C], 7);
    CHECK_EQ(raw[8], 0);   // version bytes not rewritten by the chain
}

TEST(build_pti_from_mti) {
    Instrument a;
    std::strcpy(a.name, "snare.WAV");
    a.volume = 50;
    a.lengthFrames = 999;   // stale; ignored
    a.grainLength = 441;
    uint8_t mti[0x188];
    size_t mtiLen = makeMti(mti, 6, 372, a);
    // 100-frame PCM16 mono 44.1 kHz WAV (fast path).
    int16_t pcm[100];
    for (int i = 0; i < 100; ++i) pcm[i] = int16_t(i * 300 - 15000);
    std::vector<uint8_t> wavBuf(44 + 200);
    CHECK_EQ(wavEncode(pcm, 100, 1, wavBuf.data(), wavBuf.size()), wavBuf.size());
    static int16_t scratch[1024];
    std::vector<uint8_t> out(0x188 + 1024);
    Instrument b;
    FmtStatus st;
    size_t n = buildPtiFromMti(mti, mtiLen, wavBuf.data(), wavBuf.size(), scratch, 1024, out.data(), out.size(), &b,
                               &st);
    CHECK(st == FmtStatus::Ok);
    CHECK_EQ(n, 0x188u + 200u);
    CHECK_EQ(b.lengthFrames, 100u);
    CHECK_EQ(b.grainLength, 100);
    CHECK_STR(b.name, "snare");
    CHECK(out[0] == 'T' && out[1] == 'I');
    CHECK_EQ(rdU16(out.data() + 2), 1);
    CHECK(out[4] == 1 && out[5] == 9 && out[6] == 2 && out[7] == 1);
    for (int i = 0; i < 4; ++i) CHECK_EQ(out[8 + i], 9);
    CHECK_EQ(rdU16(out.data() + 0x0C), 0x174);
    for (int i = 0; i < 100; ++i) CHECK_EQ(rdI16(out.data() + 0x188 + 2 * i), pcm[i]);
    // Round trip through the normal .pti loader.
    Instrument c;
    PtiLoadInfo li;
    CHECK(decodePtiHeader(out.data(), n, c, &li) == FmtStatus::Ok);
    CHECK(!li.upgraded);
    CHECK_EQ(c.volume, 50);   // G4(50) = 50, written as v9
    CHECK_EQ(c.lengthFrames, 100u);
    // WAV the importer rejects -> failure.
    wavBuf[0] = 'X';
    n = buildPtiFromMti(mti, mtiLen, wavBuf.data(), wavBuf.size(), scratch, 1024, out.data(), out.size(), &b, &st);
    CHECK_EQ(n, 0u);
    CHECK(st != FmtStatus::Ok);
    // Output too small.
    wavBuf[0] = 'R';
    n = buildPtiFromMti(mti, mtiLen, wavBuf.data(), wavBuf.size(), scratch, 1024, out.data(), 0x188, &b, &st);
    CHECK_EQ(n, 0u);
    CHECK(st == FmtStatus::NoSpace);
}


