// Storage-level helpers (spec 02 §0, §1) using an in-memory IStorage.
#include "test.h"

#include <vector>

#include "formats/bytes.h"
#include "formats/file_io.h"
#include "formats/patterns_metadata.h"
#include "mem_storage.h"

using namespace ot;

TEST(read_whole_file_semantics) {
    MemStorage fs;
    fs.files["a"] = {1, 2, 3};
    uint8_t buf[8];
    std::memset(buf, 0xAA, sizeof(buf));
    uint32_t got = 99;
    CHECK(readWholeFile(fs, "a", buf, 6, &got) == FmtStatus::Ok);
    CHECK_EQ(got, 3u);
    CHECK_BYTES(buf, 1, 2, 3, 0, 0, 0, 0xAA, 0xAA);  // zero-filled to `requested`
    CHECK(readWholeFile(fs, "a", buf, 2, &got) == FmtStatus::Ok);
    CHECK_EQ(got, 2u);
    CHECK(readWholeFile(fs, "nope", buf, 2, &got) == FmtStatus::NotFound);
    // Large file read in chunks across short reads.
    std::vector<uint8_t> big(100000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 13);
    fs.files["big"] = big;
    std::vector<uint8_t> out(big.size());
    CHECK(readWholeFile(fs, "big", out.data(), uint32_t(out.size()), &got) == FmtStatus::Ok);
    CHECK(out == big);
}

TEST(write_whole_file_replaces) {
    MemStorage fs;
    fs.files["x"] = std::vector<uint8_t>(50, 9);
    uint8_t d[3] = {4, 5, 6};
    CHECK(writeWholeFile(fs, "x", d, 3) == FmtStatus::Ok);
    CHECK(fs.files["x"] == std::vector<uint8_t>({4, 5, 6}));
}

TEST(paths) {
    char p[128];
    CHECK(projectFilePath(p, sizeof(p), "Workspace"));
    CHECK_STR(p, "Workspace/project.mt");
    CHECK(patternFilePath(p, sizeof(p), "Workspace", 7));
    CHECK_STR(p, "Workspace/patterns/pattern_07.mtp");
    CHECK(patternFilePath(p, sizeof(p), "Workspace", 123));
    CHECK_STR(p, "Workspace/patterns/pattern_123.mtp");
    CHECK(patternNamesPath(p, sizeof(p), "Projects/Song"));
    CHECK_STR(p, "Projects/Song/patterns/patternsMetadata");
    CHECK(workspaceInstrumentPath(p, sizeof(p), "Workspace", 1));
    CHECK_STR(p, "Workspace/instruments/instrument_01.pti");
    CHECK(savedInstrumentPath(p, sizeof(p), "Projects/Song", 3, "Kick"));
    CHECK_STR(p, "Projects/Song/instruments/3 Kick.pti");
    CHECK(!projectFilePath(p, 8, "Workspace"));
}

TEST(project_pattern_names_recents_via_storage) {
    MemStorage fs;
    static Project p, q;
    p.setTempo(111.0f);
    p.setName("Stored");
    CHECK(saveProjectFile(fs, "Workspace/project.mt", p) == FmtStatus::Ok);
    CHECK_EQ(fs.files["Workspace/project.mt"].size(), size_t(0x914));
    CHECK(loadProjectFile(fs, "Workspace/project.mt", q) == FmtStatus::Ok);
    CHECK_NEAR(q.tempo(), 111.0, 0);
    CHECK_STR(q.name, "Stored");
    CHECK(loadProjectFile(fs, "missing.mt", q) == FmtStatus::NotFound);

    static Pattern a, b;
    a.setDefault();
    a.tracks[11].steps[127].note = 99;
    a.tracks[15].steps[3].note = 77;
    CHECK(savePatternFile(fs, "Workspace/patterns/pattern_01.mtp", a) == FmtStatus::Ok);
    CHECK_EQ(fs.files["Workspace/patterns/pattern_01.mtp"].size(), size_t(0x3030));   // Tracker Mini layout
    CHECK(loadPatternFile(fs, "Workspace/patterns/pattern_01.mtp", b) == FmtStatus::Ok);
    CHECK_EQ(b.tracks[11].steps[127].note, 99);
    CHECK_EQ(b.tracks[15].steps[3].note, 77);

    static PatternNames n, m;
    n.set(2, "Verse");
    CHECK(savePatternNamesFile(fs, "pm", n) == FmtStatus::Ok);
    CHECK_EQ(fs.files["pm"].size(), size_t(0x3210));
    CHECK(loadPatternNamesFile(fs, "pm", m) == FmtStatus::Ok);
    CHECK_STR(m.get(2), "Verse");
    CHECK(loadPatternNamesFile(fs, "nope", m) == FmtStatus::NotFound);
    CHECK_STR(m.get(2), "");

    static Recents r, s;
    r.touch("Projects/A");
    CHECK(saveRecentsFile(fs, "Projects/recents", r) == FmtStatus::Ok);
    CHECK_EQ(fs.files["Projects/recents"].size(), size_t(5000));
    CHECK(loadRecentsFile(fs, "Projects/recents", s) == FmtStatus::Ok);
    CHECK_STR(s.paths[0], "Projects/A");
}

TEST(instrument_via_storage) {
    MemStorage fs;
    const uint32_t n = 3000;  // > one 0x800 write chunk
    std::vector<int16_t> planar(2 * n);
    for (uint32_t i = 0; i < 2 * n; ++i) planar[i] = int16_t(i * 11 - 30000);
    Instrument a;
    std::strcpy(a.name, "Pad");
    a.lengthFrames = n;
    a.stereo = true;
    const char* path = "Workspace/instruments/instrument_01.pti";
    CHECK(saveInstrumentFile(fs, path, a, planar.data()) == FmtStatus::Ok);
    CHECK_EQ(fs.files[path].size(), size_t(0x188 + 4 * n));

    Instrument b;
    std::vector<int16_t> bank(2 * n, 0);
    PtiSampleLayout lay;
    CHECK(loadInstrumentFile(fs, path, b, bank.data(), bank.size() - 1, true) == FmtStatus::NoSpace);
    CHECK(loadInstrumentFile(fs, path, b, bank.data(), bank.size(), true, &lay) == FmtStatus::Ok);
    CHECK(b.stereo && lay.useStereo && !lay.headerDirty);
    CHECK(bank == planar);
    // Stereo setting off: only the left block.
    std::fill(bank.begin(), bank.end(), 0);
    CHECK(loadInstrumentFile(fs, path, b, bank.data(), n, false, &lay) == FmtStatus::Ok);
    CHECK(!b.stereo && lay.stereoInFile);
    CHECK(std::equal(bank.begin(), bank.begin() + n, planar.begin()));

    // Header-only autosave keeps the payload.
    b.volume = 77;
    CHECK(saveInstrumentHeader(fs, path, b) == FmtStatus::Ok);
    CHECK_EQ(fs.files[path].size(), size_t(0x188 + 4 * n));
    CHECK_EQ(fs.files[path][0x110], 77);
    CHECK_EQ(rdI16(fs.files[path].data() + 0x188 + 2 * n), planar[n]);

    // Length mismatch is repaired (spec 02 §6.3).
    fs.files[path].resize(0x188 + 2 * 1001);
    CHECK(loadInstrumentFile(fs, path, b, bank.data(), bank.size(), true, &lay) == FmtStatus::Ok);
    CHECK(lay.headerDirty);
    CHECK_EQ(b.lengthFrames, 1001u);
    CHECK(!b.stereo);

    // Wrong type and missing files.
    fs.files[path][2] = 5;
    CHECK(loadInstrumentFile(fs, path, b, bank.data(), bank.size(), true) == FmtStatus::WrongType);
    CHECK(loadInstrumentFile(fs, "nope.pti", b, bank.data(), bank.size(), true) == FmtStatus::NotFound);
}
