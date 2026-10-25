// Fixture tests for the spec 10 legacy upgrades over the user's old SD-card
// images (fixtures/sdcard-2021, fixtures/sdcard-backup-2026-10-15). The
// files are read in place and never copied into the repo; every test skips
// cleanly when the folders are absent.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "formats/bytes.h"
#include "formats/instrument_pti.h"
#include "formats/legacy_upgrade.h"
#include "formats/pattern_mtp.h"
#include "formats/project_mt.h"
#include "formats/wav.h"
#include "model/fx.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace ot;

namespace {

std::string fixturesRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return e;
    if (const char* e = std::getenv("OT_FIXTURES")) return std::string(e) + "/../../../fixtures";
    return "../../fixtures";
}

std::vector<std::string> cardRoots() {
    std::vector<std::string> r;
    for (const char* card : {"sdcard-2021", "sdcard-backup-2026-10-15"}) {
        std::error_code ec;
        fs::path p = fs::path(fixturesRoot()) / card;
        if (fs::is_directory(p, ec)) r.push_back(p.string());
    }
    return r;
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Every folder holding a project.mt below <card>/Projects and <card>/Workspace.
std::vector<fs::path> projectDirs(const std::string& card) {
    std::vector<fs::path> out;
    for (const char* top : {"Projects", "Workspace"}) {
        std::error_code ec;
        fs::path base = fs::path(card) / top;
        if (!fs::is_directory(base, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().filename() == "project.mt") out.push_back(it->path().parent_path());
        }
    }
    return out;
}

bool skipIfAbsent() {
    if (cardRoots().empty()) {
        std::printf("  SKIP: no legacy fixtures under %s\n", fixturesRoot().c_str());
        return true;
    }
    return false;
}

struct Stats {
    std::map<int, int> mt, mtp, mti;
    int wavBuilt = 0, wavMissing = 0, wavTruncated = 0;
};
Stats g_stats;

// spec 10 §1.1 table: structure version -> size field / file length.
uint32_t legacyMtSize(uint8_t v) {
    switch (v) {
    case 2: return 1084;
    case 3: return 1100;
    case 7: return 1284;
    case 11: return 1572;
    case 17: return 0x914;
    default: return 0;
    }
}

struct LoadedProject {
    Project proj;
    Instrument ins[kSampleInstrumentCount];
};

void checkProjectFile(const fs::path& dir, LoadedProject& lp) {
    std::vector<uint8_t> f = readFile(dir / "project.mt");
    CHECK(f.size() >= 16);
    if (f.size() < 16) return;
    const uint8_t v = f[8];
    ++g_stats.mt[v];
    if (legacyMtSize(v)) {
        CHECK_EQ(rdU16(f.data() + 0x0C), legacyMtSize(v));
        CHECK_EQ(f.size(), size_t(legacyMtSize(v)));
    }
    ProjectLoadInfo info;
    CHECK(decodeProject(f.data(), f.size(), lp.proj, &info) == FmtStatus::Ok);
    CHECK_EQ(info.upgraded, v < 17);
    if (v < 17) {
        // spec 10 §1.3 / §5.1: every legacy file takes the "Unsaved Project" path.
        CHECK(info.sizeMismatch);
        CHECK_STR(lp.proj.name, "Unsaved Project");
        CHECK_EQ(lp.proj.u8(0x140), 3);
        // Raw image end state (before the sanitiser).
        static uint8_t img[mt::kFileSize];
        std::memset(img, 0, sizeof(img));
        std::memcpy(img, f.data(), f.size() < sizeof(img) ? f.size() : sizeof(img));
        CHECK(legacy::upgradeProjectImage(img, v));
        CHECK(std::memcmp(img, f.data(), 16) == 0);   // header untouched
        CHECK_EQ(img[0x80C], 0);
        CHECK_EQ(rdU16(img + 0x5FE), 20000);
        CHECK_EQ(rdU16(img + 0x600), 0);
        CHECK_EQ(img[0x602], 0);
        CHECK_STR(reinterpret_cast<const char*>(img + 0x603), "Midi 9");
        CHECK_STR(reinterpret_cast<const char*>(img + 0x603 + 7 * 8), "Midi 16");
        for (uint32_t i = 0x643; i <= 0x749; ++i)
            if (img[i]) {
                CHECK_EQ(img[i], 0);
                break;
            }
        if (v <= 10) {
            CHECK_EQ(img[0x116], 11);
            CHECK_EQ(img[0x117], 24);
            for (int i = 0; i < 16; ++i) CHECK_EQ(img[0x4FD + 16 * i], i);
        }
        if (v <= 4)   // spec 10 §1.4: levels end at 86
            for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(img[0x4D0 + i], 86);
        if (v <= 3) CHECK_STR(reinterpret_cast<const char*>(img + 0x428), "Track 1");
        // The workspace copy written on open is a v17 0x914-byte file.
        static uint8_t out[mt::kFileSize];
        encodeProject(lp.proj, out);
        CHECK_EQ(out[8], 17);
        CHECK_EQ(rdU16(out + 0x0C), 0x914);
    }
}

void checkInstruments(const fs::path& dir, LoadedProject& lp) {
    for (int n = 1; n <= kSampleInstrumentCount; ++n) {
        char name[40];
        std::snprintf(name, sizeof(name), "instrument_%02d.mti", n);
        fs::path p = dir / "instruments" / name;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        std::vector<uint8_t> f = readFile(p);
        CHECK(f.size() >= 16);
        if (f.size() < 16) continue;
        const uint8_t v = f[8];
        ++g_stats.mti[v];
        // spec 10 §3.1 table.
        const uint16_t body = rdU16(f.data() + 0x0C);
        CHECK_EQ(body, v <= 3 ? 368 : 372);
        CHECK_EQ(f.size(), size_t(body) + 0x14);
        Instrument& ins = lp.ins[n - 1];
        PtiLoadInfo info;
        CHECK(decodePtiHeader(f.data(), f.size(), ins, &info) == FmtStatus::Ok);
        CHECK(info.upgraded);
        CHECK(legacy::ptiNeedsWav(f.size(), info.dataOffset));
        CHECK_EQ(ins.bitDepth, 16);   // every legacy version passes step 6->7
        CHECK_EQ(ins.reserved173, 0);
        if (v <= 5) CHECK_EQ(ins.overdrive, 0);
        CHECK(ins.volume <= 100);

        char wname[24];
        std::snprintf(wname, sizeof(wname), "instr%02d.wav", n);
        fs::path wp = dir / "samples" / wname;
        if (!fs::is_regular_file(wp, ec)) {
            ++g_stats.wavMissing;
            continue;
        }
        std::vector<uint8_t> w = readFile(wp);
        WavInfo wi;
        CHECK(wavParseHeader(w.data(), w.size(), wi));
        // spec 10 §5.3: all PCM16 mono 44.1 kHz -> fast path.
        CHECK(wavIsFastPath(wi));
        // Memory-check length (spec 10 §3.3 step 3) vs. what the importer
        // produces from the declared data size (some WAVs are truncated).
        const uint32_t checkFrames = legacy::legacyWavCheckFrames(wi, uint32_t(w.size()));
        const uint32_t frames = wavPlanImport(wi, WavImportOptions{}).outFrames;
        if (checkFrames < frames) ++g_stats.wavTruncated;
        std::vector<int16_t> scratch(size_t(frames) + 16);
        std::vector<uint8_t> out(0x188 + size_t(frames) * 2 + 16);
        Instrument built;
        FmtStatus st = FmtStatus::IoError;
        size_t sz = legacy::buildPtiFromMti(f.data(), f.size(), w.data(), w.size(), scratch.data(), scratch.size(),
                                            out.data(), out.size(), &built, &st);
        CHECK(st == FmtStatus::Ok);
        if (st != FmtStatus::Ok) continue;
        ++g_stats.wavBuilt;
        CHECK_EQ(sz, 0x188 + size_t(built.lengthFrames) * 2);
        CHECK_EQ(built.lengthFrames, frames);
        CHECK(out[8] == 9 && out[9] == 9 && out[10] == 9 && out[11] == 9);
        Instrument back;
        PtiLoadInfo li;
        CHECK(decodePtiHeader(out.data(), sz, back, &li) == FmtStatus::Ok);
        CHECK(!li.upgraded);
        CHECK_EQ(back.volume, ins.volume);
        CHECK_EQ(back.playMode, ins.playMode);
        // A name ending in ".WAV.WAV" loses one suffix per load (spec 10 §3.3 step 2).
        CHECK(std::strncmp(back.name, ins.name, std::strlen(back.name)) == 0);
        PtiSampleLayout lay = resolvePtiSample(back.lengthFrames, uint32_t(sz - li.dataOffset), false);
        CHECK(!lay.headerDirty);
    }
}

struct PatternCounts {
    int volCells = 0, volChanged = 0, beatSlice = 0;
};

void checkPatterns(const fs::path& dir, LoadedProject& lp, PatternCounts& pc) {
    uint8_t modes[kSampleInstrumentCount];
    for (int i = 0; i < kSampleInstrumentCount; ++i) modes[i] = lp.ins[i].playMode;
    PatternUpgradeContext ctx;
    ctx.instrumentPlayModes = modes;
    ctx.beatSliceReference = lp.proj.u8(0x117);
    static Pattern up, raw;
    for (int n = 1; n <= 255; ++n) {
        char name[40];
        std::snprintf(name, sizeof(name), "pattern_%02d.mtp", n);
        fs::path p = dir / "patterns" / name;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        std::vector<uint8_t> f = readFile(p);
        CHECK(f.size() >= 16);
        if (f.size() < 16) continue;
        const uint8_t v = f[8];
        ++g_stats.mtp[v];
        PatternLoadInfo info;
        decodePattern(f.data(), f.size(), up, &ctx, &info);
        if (v >= 5 && rdU16(f.data() + 0x0C) == mtp::kFileSize) continue;
        // spec 10 §2.1: legacy files are 6184 bytes, 8 tracks.
        CHECK_EQ(f.size(), size_t(mtp::kLegacyFileSize));
        CHECK_EQ(rdU16(f.data() + 0x0C), mtp::kLegacyFileSize);
        CHECK(info.upperTracksReset);
        for (int t = 8; t < 12; ++t) {
            CHECK_EQ(up.tracks[t].lastStep, up.tracks[0].lastStep);
            for (auto& s : up.tracks[t].steps)
                if (!(s.note == kNoteEmpty && s.instrument == 0 && s.fx[0].type == 0 && s.fx[1].type == 0 &&
                      s.fx[0].value == 0 && s.fx[1].value == 0)) {
                    CHECK(false);
                    break;
                }
        }
        // Count Volume-FX cells and beat-slice notes against the raw file.
        std::vector<uint8_t> asIs = f;
        std::memset(asIs.data() + 8, 5, 4);
        decodePattern(asIs.data(), asIs.size(), raw, nullptr, nullptr);
        for (int t = 0; t < 8; ++t) {
            const Track& rt = raw.tracks[t];
            if (rt.lastStep == 0xFF) continue;
            for (int s = 0; s <= rt.lastStep && s < kStepCount; ++s) {
                const Step& a = rt.steps[s];
                const Step& b = up.tracks[t].steps[s];
                if (!isSampleInstrument(a.instrument)) continue;
                for (int k = 0; k < 2; ++k)
                    if (a.fx[k].type == uint8_t(FxId::Volume) && v < 4) {
                        ++pc.volCells;
                        if (a.fx[k].value != b.fx[k].value) ++pc.volChanged;
                    }
                if (a.note != b.note) ++pc.beatSlice;
            }
        }
    }
}

}  // namespace

TEST(legacy_fixture_cards) {
    if (skipIfAbsent()) return;
    for (const std::string& card : cardRoots()) {
        std::vector<fs::path> dirs = projectDirs(card);
        std::printf("  %s: %zu project folders\n", card.c_str(), dirs.size());
        for (const fs::path& d : dirs) {
            static LoadedProject lp;
            lp = LoadedProject{};
            checkProjectFile(d, lp);
            checkInstruments(d, lp);
            PatternCounts pc;
            checkPatterns(d, lp, pc);
            const std::string name = d.filename().string();
            if (card.find("backup") != std::string::npos && d.parent_path().filename() == "Projects") {
                // spec 10 §5.2 examples.
                if (name.rfind("Barker", 0) == 0) {
                    CHECK_EQ(pc.volCells, 64);
                    CHECK_EQ(pc.volChanged, 62);
                }
                if (name.rfind("Dataline", 0) == 0) {
                    CHECK_EQ(pc.volCells, 224);
                    CHECK_EQ(pc.volChanged, 224);
                }
                if (name == "lucky plastic") {
                    CHECK_EQ(pc.volCells, 110);
                    CHECK_EQ(pc.volChanged, 108);
                }
            }
            if (pc.beatSlice) std::printf("    %s: %d beat-slice notes transposed\n", name.c_str(), pc.beatSlice);
        }
    }
    auto dump = [](const char* what, const std::map<int, int>& m) {
        std::printf("  %s versions:", what);
        for (auto& kv : m) std::printf(" v%d x%d", kv.first, kv.second);
        std::printf("\n");
    };
    dump(".mt", g_stats.mt);
    dump(".mtp", g_stats.mtp);
    dump(".mti", g_stats.mti);
    std::printf("  .mti+wav -> .pti built: %d (wav missing: %d, data chunk past EOF: %d)\n", g_stats.wavBuilt,
                g_stats.wavMissing, g_stats.wavTruncated);
    CHECK(!g_stats.mt.empty());
}
