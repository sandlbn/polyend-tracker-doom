// 2.x M1-WP1 (docs/M1_PLAN.md §1): 16-track working Pattern, 12-track
// PatternStock store and the ext-track pool for tracks 13..16.
#include <cstring>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/pattern_mtp.h"
#include "model/pattern_store.h"
#include "sequencer/sequencer.h"

using namespace ot;

// ---- memory budget (mirrors the static_asserts of src/app_main.cpp) --------
static_assert(sizeof(Track) == 769, "track block");
static_assert(sizeof(PatternStock) == 9228, "12 tracks");
static_assert(sizeof(Pattern) == 12304, "16 tracks");
static_assert(sizeof(PerfPatternMemory) == 769 * 16 * 2 + sizeof(Pattern), "perf slots [16][2] + scratch");
static_assert(ExtTrackPool::kDefaultBlocks == 384, "spec gap 9");
static_assert(ExtTrackPool::metaBytes(384) == 255 * 4 * 2 + 48, "uint16 index + used bitmap");
namespace {
constexpr uint32_t al(uint32_t v) { return (v + 15u) & ~15u; }
constexpr uint32_t kFx = 3u * 262144u * 2u + 2u * 16384u * 2u;
constexpr uint32_t kP = sizeof(Pattern);
constexpr uint32_t kSdramUsed =
    al(al(al(al(al(al(al(al(al(al(al(kSampleBankBytes) + kFx) + sizeof(PatternStock) * kPatternCount) +
                         ExtTrackPool::blockBytes(ExtTrackPool::kDefaultBlocks + 1)) +
                      ExtTrackPool::metaBytes(ExtTrackPool::kDefaultBlocks)) +
                   2 * kP) +
                20 * kP) +
             10 * kP) +
          kP) +
       64) +
       0x10000) +
    sizeof(PerfPatternMemory);
}  // namespace
static_assert(16u * 1024u * 1024u - kSdramUsed >= 150u * 1024u, "M1: >= 150 KB SDRAM left");

namespace {
Track g_blocks[4];
alignas(2) uint8_t g_metaAligned[ExtTrackPool::metaBytes(3)];

Track filled(uint8_t note, uint8_t last = 31) {
    Track t;
    t.clear(last);
    t.steps[3].note = int8_t(note);
    t.steps[3].instrument = 50;
    return t;
}

void fillRandom(PatternStock& p, uint32_t seed) {
    for (auto& t : p.tracks) {
        t.lastStep = uint8_t((seed >> 3) % 128);
        for (auto& st : t.steps) {
            seed = seed * 1664525u + 1013904223u;
            st.note = int8_t(int(seed >> 24) % 132 - 4);
            st.instrument = uint8_t((seed >> 8) % 64);
            st.fx[0] = {uint8_t((seed >> 12) % 43), uint8_t(seed >> 16)};
            st.fx[1] = {uint8_t((seed >> 4) % 43), uint8_t(seed >> 20)};
        }
    }
}
}  // namespace

TEST(constants_and_types) {
    CHECK_EQ(kTrackCount, 16);
    CHECK_EQ(kStockTrackCount, 12);
    CHECK_EQ(kExtTrackCount, 4);
    CHECK_EQ(kMidiTrackStart, 8);
    CHECK_EQ(int(Pattern::kTracks), 16);
    CHECK_EQ(int(PatternStock::kTracks), 12);
    Track t;
    t.clear(31);
    CHECK(t.blank());
    CHECK(extTrackIsDefault(t, 31));
    CHECK(!extTrackIsDefault(t, 63));   // own length differs: keep the block
    t.steps[0].fx[1].value = 9;         // FX value without a type: still blank
    CHECK(t.blank());
    t.steps[5].instrument = 3;
    CHECK(!t.blank());
}

TEST(pool_alloc_free_cap) {
    ExtTrackPool pool;
    pool.attach(g_blocks, 3, g_metaAligned);
    CHECK(pool.attached());
    CHECK_EQ(pool.capacity(), 3);
    CHECK_EQ(pool.used(), 0);
    CHECK(pool.find(1, 0) == nullptr);
    CHECK_EQ(int(pool.blockOf(1, 0)), int(ExtTrackPool::kNone));

    Track* a = pool.alloc(1, 0, filled(60));
    CHECK(a != nullptr);
    CHECK_EQ(int(a->steps[3].note), 60);
    CHECK(pool.find(1, 0) == a);
    CHECK(pool.alloc(1, 0, filled(61)) == a);   // existing block, not re-initialised
    CHECK_EQ(int(a->steps[3].note), 60);
    Track* b = pool.alloc(255, 3, filled(62));
    Track* c = pool.alloc(7, 2, filled(63));
    CHECK(b && c && a != b && b != c && a != c);
    CHECK_EQ(pool.used(), 3);
    CHECK_EQ(pool.freeBlocks(), 0);

    // Full: refused and counted, nothing overwritten.
    CHECK(pool.alloc(2, 1, filled(64)) == nullptr);
    CHECK_EQ(int(pool.fullEvents()), 1);
    CHECK(pool.find(2, 1) == nullptr);
    CHECK_EQ(int(a->steps[3].note), 60);
    CHECK_EQ(int(b->steps[3].note), 62);

    // Out of range never allocates.
    CHECK(pool.alloc(0, 0, filled(1)) == nullptr);
    CHECK(pool.alloc(256, 0, filled(1)) == nullptr);
    CHECK(pool.alloc(1, 4, filled(1)) == nullptr);
    CHECK(pool.find(0, 0) == nullptr);

    // Free one, reuse it.
    pool.release(255, 3);
    CHECK(pool.find(255, 3) == nullptr);
    CHECK_EQ(pool.used(), 2);
    Track* d = pool.alloc(2, 1, filled(65));
    CHECK(d == b);
    CHECK_EQ(int(d->steps[3].note), 65);
    pool.release(9, 0);   // no block: no-op
    CHECK_EQ(pool.used(), 3);

    pool.releasePattern(7);
    CHECK_EQ(pool.used(), 2);
    pool.clear();
    CHECK_EQ(pool.used(), 0);
    CHECK_EQ(int(pool.fullEvents()), 0);
    CHECK(pool.find(1, 0) == nullptr && pool.find(2, 1) == nullptr);

    // Null pool: reads empty, allocations fail, no crash.
    ExtTrackPool none;
    none.attach(nullptr, 0, nullptr);
    CHECK(!none.attached());
    CHECK(none.find(1, 0) == nullptr);
    CHECK(none.alloc(1, 0, filled(1)) == nullptr);
    CHECK_EQ(none.capacity(), 0);
}

TEST(codec_stock_bytes_unchanged) {
    // The .mtp image is the 12 stock tracks: a 16-track Pattern with data in
    // tracks 13..16 encodes to the same bytes as its PatternStock.
    static PatternStock s;
    static Pattern p;
    static uint8_t a[mtp::kFileSize], b[mtp::kFileSize];
    fillRandom(s, 1234);
    p.copyStockFrom(s);
    for (int t = kStockTrackCount; t < kTrackCount; ++t) p.tracks[t] = filled(uint8_t(70 + t), 99);
    encodePattern(s, a);
    encodePattern(p, b);
    CHECK(std::memcmp(a, b, sizeof(a)) == 0);
    // Decode into a Pattern: stock tracks as stored, ext tracks reset.
    static Pattern q;
    decodePattern(a, sizeof(a), q);
    bool same = true;
    for (int t = 0; t < kStockTrackCount; ++t) {
        same = same && q.tracks[t].lastStep == s.tracks[t].lastStep;
        for (int k = 0; k < kStepCount; ++k) same = same && q.tracks[t].steps[k] == s.tracks[t].steps[k];
    }
    CHECK(same);
    for (int t = kStockTrackCount; t < kTrackCount; ++t) CHECK(extTrackIsDefault(q.tracks[t], s.tracks[0].lastStep));
    static PatternStock r;
    decodePattern(a, sizeof(a), r);
    encodePattern(r, b);
    CHECK(std::memcmp(a, b, sizeof(a)) == 0);
}

TEST(store_alloc_on_write_and_compact) {
    Harness h;
    App& a = *h.app;
    const ExtTrackPool& pool = a.extPool();
    CHECK_EQ(pool.capacity(), ExtTrackPool::kDefaultBlocks);
    CHECK_EQ(pool.used(), 0);

    // Read access never allocates.
    CHECK(a.findStoredTrack(5, 13) == nullptr);
    CHECK(a.findStoredTrack(5, 3) == &a.storedStock(5).tracks[3]);
    Track tr;
    a.readStoredTrack(5, 14, tr);
    CHECK(extTrackIsDefault(tr, a.storedStock(5).tracks[0].lastStep));
    CHECK_EQ(pool.used(), 0);

    // storedTrack: stock tracks are resident, ext tracks allocate.
    CHECK(&a.storedTrack(5, 11) == &a.storedStock(5).tracks[11]);
    CHECK_EQ(pool.used(), 0);
    Track& t13 = a.storedTrack(5, 12);
    CHECK_EQ(pool.used(), 1);
    CHECK(extTrackIsDefault(t13, a.storedStock(5).tracks[0].lastStep));
    t13.steps[0].note = 48;
    t13.steps[0].instrument = 50;
    CHECK(&a.storedTrack(5, 12) == &t13);
    CHECK_EQ(pool.used(), 1);
    CHECK(a.findStoredTrack(5, 12) == &t13);

    // Assembly: 16 tracks.
    static Pattern p;
    a.assemblePattern(5, p);
    CHECK_EQ(int(p.tracks[12].steps[0].note), 48);
    CHECK(p.tracks[13].blank());
    CHECK_EQ(int(p.tracks[13].lastStep), int(a.storedStock(5).tracks[0].lastStep));

    // storePattern: an empty ext track takes no block; data does.
    p.tracks[15].steps[7].note = 40;
    p.tracks[15].steps[7].instrument = 63;
    p.tracks[2].steps[1].note = 30;
    CHECK(a.storePattern(5, p));
    CHECK_EQ(pool.used(), 2);
    CHECK(a.findStoredTrack(5, 13) == nullptr);
    CHECK(a.findStoredTrack(5, 14) == nullptr);
    CHECK_EQ(int(a.findStoredTrack(5, 15)->steps[7].note), 40);
    CHECK_EQ(int(a.storedStock(5).tracks[2].steps[1].note), 30);

    // Emptied tracks keep their block until compactExtTracks().
    p.tracks[12].steps[0] = Step{};
    p.tracks[15].steps[7] = Step{};
    CHECK(a.storePattern(5, p));
    CHECK_EQ(pool.used(), 2);
    CHECK_EQ(a.compactExtTracks(9), 0);   // other pattern
    CHECK_EQ(a.compactExtTracks(5), 2);
    CHECK_EQ(pool.used(), 0);
    CHECK(a.findStoredTrack(5, 12) == nullptr);

    // compactExtTracks(0) walks every pattern; a non-default length keeps a block.
    a.storedTrack(3, 13).lastStep = 7;
    a.storedTrack(200, 14);
    CHECK_EQ(pool.used(), 2);
    CHECK_EQ(a.compactExtTracks(), 1);
    CHECK(a.findStoredTrack(3, 13) != nullptr);
    CHECK(a.findStoredTrack(200, 14) == nullptr);

    // Out of range: no allocation, no crash.
    CHECK(a.findStoredTrack(0, 12) == nullptr);
    CHECK(a.findStoredTrack(1, 16) == nullptr);
    CHECK(!a.storedTrackWritable(256, 12));
    a.storedTrack(0, 12).steps[0].note = 1;   // discarded
    CHECK_EQ(pool.used(), 1);

    a.clearExtTracks(3);
    CHECK_EQ(pool.used(), 0);
}

TEST(working_pattern_round_trip_and_load) {
    Harness h;
    App& a = *h.app;
    // Edit tracks 1 and 16 of the current pattern, commit, switch away and back.
    a.pattern().tracks[0].steps[0].note = 60;
    a.pattern().tracks[15].steps[2].note = 61;
    a.pattern().tracks[15].steps[2].instrument = 63;
    a.patternEdited();
    CHECK_EQ(a.extPool().used(), 1);
    CHECK(a.patternHasData(1));
    a.selectPattern(2);
    CHECK_EQ(int(a.patternNumber()), 2);
    CHECK(a.pattern().tracks[15].blank());
    a.selectPattern(1);
    CHECK_EQ(int(a.pattern().tracks[0].steps[0].note), 60);
    CHECK_EQ(int(a.pattern().tracks[15].steps[2].note), 61);

    // A pattern with data only on track 16 still has data.
    a.storedTrack(9, 15).steps[0].note = 50;
    a.markPatternDirty(9);
    CHECK(a.patternHasData(9));

    // Audio-context load: assembles, never allocates.
    static Pattern dst;
    const int used = a.extPool().used();
    for (int n = 1; n <= kPatternCount; ++n) a.loadPattern(uint8_t(n), dst, false);
    CHECK_EQ(a.extPool().used(), used);
    a.loadPattern(9, dst, false);
    CHECK_EQ(int(dst.tracks[15].steps[0].note), 50);
    // Spread preload (CPU round 3): loading by track gives the same bytes,
    // for present patterns (with and without ext tracks) and default ones.
    // (A default ext track keeps the buffer's FX values, Step::clear(): both
    // buffers start with the same bytes.)
    static Pattern part;
    for (int n : {1, 2, 9, 200}) {
        std::memset(static_cast<void*>(&part), 0x5A, sizeof part);
        std::memset(static_cast<void*>(&dst), 0x5A, sizeof dst);
        a.loadPattern(uint8_t(n), dst, false);
        for (int t = 0; t < kTrackCount; t += 3) a.loadPatternTracks(uint8_t(n), part, t, 3);
        CHECK(std::memcmp(&part, &dst, sizeof part) == 0);
    }

    // Undo snapshots carry all 16 tracks.
    a.pushUndo();
    a.pattern().tracks[14].steps[0].note = 33;
    a.patternEdited();
    CHECK(a.undoEdit());
    CHECK(a.pattern().tracks[14].blank());
    CHECK(a.redoEdit());
    CHECK_EQ(int(a.pattern().tracks[14].steps[0].note), 33);

    // File undo: the stored pattern with its ext tracks, in place.
    a.pushFileUndo(9);
    a.storedTrack(9, 15).steps[0].note = 51;
    a.storedStock(9).tracks[0].steps[0].note = 20;
    uint8_t num = 0;
    CHECK(a.popFileUndo(&num));
    CHECK_EQ(int(num), 9);
    CHECK_EQ(int(a.findStoredTrack(9, 15)->steps[0].note), 50);
    CHECK(a.storedStock(9).tracks[0].steps[0].note < 0);
    CHECK(!a.popFileUndo(&num));

    // Clipboard is a 16-track pattern.
    SelRect r;
    a.copyToClipboard(r);
    CHECK_EQ(int(a.clipboard().tracks[14].steps[0].note), 33);
}

TEST(pool_full_reports_never_crashes) {
    Harness h;
    App& a = *h.app;
    const int cap = a.extPool().capacity();
    int n = 1, e = 0;
    for (int k = 0; k < cap; ++k) {
        a.storedTrack(n, kStockTrackCount + e).steps[0].note = 1;
        if (++e == kExtTrackCount) {
            e = 0;
            ++n;
        }
    }
    CHECK_EQ(a.extPool().used(), cap);
    CHECK_EQ(int(a.extPoolFullEvents()), 0);
    // Next ext track: refused, reported, writes go to the discard track.
    CHECK(!a.storedTrackWritable(250, 12));
    CHECK(a.storedTrackWritable(250, 3));    // stock tracks always
    CHECK(a.storedTrackWritable(1, 12));     // existing block
    Track& sink = a.storedTrack(250, 12);
    sink.steps[0].note = 99;
    CHECK(a.findStoredTrack(250, 12) == nullptr);
    CHECK(a.extPoolFullEvents() >= 1);
    // storePattern: stock tracks stored, ext overflow reported via false.
    static Pattern p;
    a.assemblePattern(251, p);
    p.tracks[0].steps[0].note = 10;
    p.tracks[13].steps[0].note = 11;
    CHECK(!a.storePattern(251, p));
    CHECK_EQ(int(a.storedStock(251).tracks[0].steps[0].note), 10);
    CHECK(a.findStoredTrack(251, 13) == nullptr);
    // Freeing makes room again.
    a.storedTrack(1, 12).steps[0] = Step{};
    CHECK_EQ(a.compactExtTracks(1), 1);
    CHECK(a.storePattern(251, p));
    CHECK_EQ(int(a.findStoredTrack(251, 13)->steps[0].note), 11);
    a.ui.update(h.clock.ms);
    a.ui.renderIfDirty(h.clock.ms);   // the notice draws
}

TEST(autosave_compacts_and_stock_files_unchanged) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[1].steps[4].note = 62;
    a.patternEdited();
    a.autosave();
    const std::vector<uint8_t> before = h.fs.files["Workspace/patterns/pattern_01.mtp"];
    CHECK_EQ(before.size(), size_t(mtp::kMiniFileSize));   // Tracker Mini 16-track file
    // Data on track 13 goes into the same file; tracks 1-12 stay as they were.
    a.pattern().tracks[12].steps[0].note = 64;
    a.pattern().tracks[12].steps[0].instrument = 60;
    a.patternEdited();
    CHECK_EQ(a.extPool().used(), 1);
    a.autosave();
    const std::vector<uint8_t>& mid = h.fs.files["Workspace/patterns/pattern_01.mtp"];
    CHECK_EQ(mid.size(), size_t(mtp::kMiniFileSize));
    CHECK(std::memcmp(mid.data(), before.data(), mtp::kTracksOff + 12 * mtp::kTrackSize) == 0);
    CHECK_EQ(int(int8_t(mid[mtp::kTracksOff + 12 * mtp::kTrackSize + 1])), 64);
    CHECK_EQ(a.extPool().used(), 1);
    // Cleared again: the autosave frees the block.
    a.pattern().tracks[12].steps[0] = Step{};
    a.patternEdited();
    a.autosave();
    CHECK_EQ(a.extPool().used(), 0);
    CHECK(h.fs.files["Workspace/patterns/pattern_01.mtp"] == before);
    // New project: pool emptied.
    a.storedTrack(4, 14).steps[0].note = 5;
    CHECK(a.newProject());
    CHECK_EQ(a.extPool().used(), 0);
}
