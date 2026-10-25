// Project-load performance proxy (docs/PLAN_2X.md performance workstream,
// "SD and loading"). Every project of the user's SD-card backup
// (fixtures/sdcard-backup-2026-10-15/Projects, read in place; skipped when
// absent) is opened through the background job on a simulated card whose
// clock advances by a FatFs/SD cost model, then reloaded through the boot
// path. Printed per project: main-loop passes, modelled job time, the
// longest pass, SD commands, and an FNV-1a hash of everything the load
// produced (project, patterns, names, instrument headers, sample bank and
// the Workspace files) so a change can be checked for byte-identical results.
//
// Cost model (Tracker SDHC: ADMA2 4-bit 45 MHz, FAT32 32 KB clusters):
//  - read command 80 us + 25 us per 512-byte sector; write command 600 us
//    + 50 us per sector (single-sector writes dominate small-file traffic);
//  - FatFs one-sector window (fs->win) shared by directory and FAT access;
//    a lookup walks each path component's directory from its first entry
//    (a missing name scans the whole directory); LFN names take 1 + len/13
//    slots; a create of a name needing a numbered short name ("~N") scans
//    the directory once per colliding "~1".."~5" candidate (FatFs gen_numname);
//  - f_read/f_write: unaligned head/tail sectors go through the file's sector
//    buffer, aligned runs are one multi-block command per cluster; crossing a
//    cluster costs a FAT lookup through the window unless fast seek (CLMT)
//    is enabled (OT_PERF_FASTSEEK=1);
//  - CPU: host time spent in the App between storage calls x kCpuScale (a
//    rough host(-O1, sanitizers) -> Cortex-M4 @180 MHz factor).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_io.h"
#include "formats/file_io.h"
#include "formats/instrument_pti.h"
#include "formats/pattern_mtp.h"
#include "formats/patterns_metadata.h"
#include "formats/project_mt.h"

using namespace ot;
namespace sfs = std::filesystem;

namespace {

constexpr double kReadCmdUs = 80, kReadSecUs = 25, kWriteCmdUs = 600, kWriteSecUs = 50;
constexpr uint32_t kCluster = 32768, kSecPerCl = kCluster / 512;
double kCpuScale = 3.0;   // OT_PERF_CPU overrides

std::string backupRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return std::string(e) + "/sdcard-backup-2026-10-15";
    if (const char* e = std::getenv("OT_FIXTURES")) return std::string(e) + "/../../../fixtures/sdcard-backup-2026-10-15";
    return "../../fixtures/sdcard-backup-2026-10-15";
}

class SimClock : public IClock {
public:
    double us = 1e9;
    uint32_t micros() const override { return uint32_t(uint64_t(us)); }
    uint32_t millis() const override { return uint32_t(uint64_t(us) / 1000); }
};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// 8.3-compatible (no LFN, no numbered short name)?
bool fits83(const std::string& n) {
    const size_t dot = n.rfind('.');
    const std::string base = dot == std::string::npos ? n : n.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
    for (char c : n)
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') return false;
    return std::count(n.begin(), n.end(), '.') <= 1;
}
int slotsFor(const std::string& n) { return fits83(n) ? 1 : 1 + int((n.size() + 12) / 13); }
std::string numPrefix(const std::string& n) {
    std::string u;
    for (char c : n) {
        if (c == '.') break;
        if (c != ' ' && c != '_') u += char(std::toupper(static_cast<unsigned char>(c)));
        if (u.size() == 6) break;
    }
    return u + "|" + lower(n.substr(n.rfind('.') == std::string::npos ? n.size() : n.rfind('.')));
}

struct SimCard : FakeStorage {
    SimClock* clock = nullptr;
    bool fastSeek = false;
    uint64_t cmds = 0, sectors = 0;
    std::chrono::steady_clock::time_point hostMark;
    bool cpuOn = false;

    struct Ent {
        std::string name;
        int slots;
        bool alive;
        uint32_t cluster;
    };
    std::map<std::string, std::vector<Ent>> dmod;   // dir path -> entry slots in order
    uint32_t nextCluster = 1000;
    struct Win {
        int kind = 0;   // 0 none, 1 dir, 2 fat
        std::string dir;
        uint32_t sec = 0;
        bool dirty = false;
        bool operator==(const Win& o) const { return kind == o.kind && dir == o.dir && sec == o.sec; }
    } win;
    struct H {
        std::string path;
        uint32_t pos = 0, size = 0, cluster = 0;
        int64_t bufSec = -1;
        bool bufDirty = false, written = false;
        int64_t curCl = -1;
    };
    std::map<Handle, H> hs;

    // ---- cost primitives
    void rd(uint32_t n) {
        clock->us += kReadCmdUs + kReadSecUs * n;
        ++cmds;
        sectors += n;
    }
    void wr(uint32_t n) {
        clock->us += kWriteCmdUs + kWriteSecUs * n;
        ++cmds;
        sectors += n;
    }
    void winTo(const Win& w, bool willDirty = false) {
        if (!(win == w)) {
            if (win.dirty) wr(1);
            win = w;
            win.dirty = false;
            rd(1);
        }
        if (willDirty) win.dirty = true;
    }
    void fatAccess(uint32_t cluster, bool dirty = false) {
        Win w;
        w.kind = 2;
        w.sec = cluster / 128;
        winTo(w, dirty);
    }
    void dirSectors(const std::string& dir, uint32_t fromSlot, uint32_t toSlot) {
        for (uint32_t s = fromSlot / 16; s <= toSlot / 16; ++s) {
            Win w;
            w.kind = 1;
            w.dir = dir;
            w.sec = s;
            winTo(w);
        }
    }
    static void split(const std::string& p, std::string& par, std::string& name) {
        const size_t s = p.rfind('/');
        par = s == std::string::npos ? "" : p.substr(0, s);
        name = s == std::string::npos ? p : p.substr(s + 1);
    }
    uint32_t totalSlots(const std::string& dir) {
        uint32_t n = 0;
        for (auto& e : dmod[dir]) n += uint32_t(e.slots);
        return n;
    }
    // Find name in dir: index into dmod[dir] or -1; charges the scanned sectors.
    int find(const std::string& dir, const std::string& name) {
        auto& v = dmod[dir];
        uint32_t slot = 0;
        const std::string ln = lower(name);
        for (size_t i = 0; i < v.size(); ++i) {
            slot += uint32_t(v[i].slots);
            if (v[i].alive && lower(v[i].name) == ln) {
                dirSectors(dir, 0, slot - 1);
                return int(i);
            }
        }
        dirSectors(dir, 0, slot ? slot - 1 : 0);
        return -1;
    }
    // Walk every component of p (parents included). True when p exists.
    bool walk(const std::string& p) {
        if (p.empty()) return true;
        std::string cur;
        size_t start = 0;
        for (;;) {
            const size_t s = p.find('/', start);
            const std::string comp = p.substr(start, s == std::string::npos ? std::string::npos : s - start);
            if (find(cur, comp) < 0) return false;
            if (s == std::string::npos) return true;
            cur = p.substr(0, s);
            start = s + 1;
        }
    }
    void registerEntry(const std::string& dir, const std::string& name) {
        if (!fits83(name)) {
            // gen_numname: "~1".."~5" each checked with a full directory scan.
            int similar = 0;
            const std::string pre = numPrefix(name);
            for (auto& e : dmod[dir])
                if (e.alive && !fits83(e.name) && numPrefix(e.name) == pre) ++similar;
            const int scans = std::min(similar, 5) + 1;
            const uint32_t all = totalSlots(dir);
            for (int k = 0; k < scans; ++k) dirSectors(dir, 0, all ? all - 1 : 0);
        }
        auto& v = dmod[dir];
        const int need = slotsFor(name);
        uint32_t slot = 0;
        size_t i = 0;
        for (; i < v.size(); ++i) {
            if (!v[i].alive && v[i].slots >= need) break;
            slot += uint32_t(v[i].slots);
        }
        dirSectors(dir, 0, slot + uint32_t(need) - 1);
        win.dirty = true;
        Ent e{name, need, true, 0};
        if (i < v.size()) {
            const int rest = v[i].slots - need;
            v[i] = e;
            if (rest > 0) v.insert(v.begin() + long(i) + 1, Ent{"", rest, false, 0});
        } else {
            v.push_back(e);
        }
    }
    void freeChain(uint32_t cluster, uint32_t bytes) {
        const uint32_t cls = (bytes + kCluster - 1) / kCluster;
        for (uint32_t c = 0; c < cls; c += 128) fatAccess(cluster + c, true);
    }
    Ent* ent(const std::string& path) {
        std::string par, name;
        split(path, par, name);
        for (auto& e : dmod[par])
            if (e.alive && lower(e.name) == lower(name)) return &e;
        return nullptr;
    }

    // ---- CPU proxy
    void enter() {
        if (cpuOn) {
            const auto now = std::chrono::steady_clock::now();
            clock->us += kCpuScale * double(std::chrono::duration_cast<std::chrono::nanoseconds>(now - hostMark).count()) / 1000.0;
        }
    }
    void leave() { hostMark = std::chrono::steady_clock::now(); }
    struct Guard {
        SimCard& c;
        explicit Guard(SimCard& s) : c(s) { c.enter(); }
        ~Guard() { c.leave(); }
    };

    void buildModel() {
        dmod.clear();
        dmod[""];
        for (auto& d : dirs) {
            std::string par, name;
            split(d, par, name);
            dmod[par].push_back(Ent{name, slotsFor(name), true, nextCluster++});
            dmod[d];
        }
        for (auto& f : files) {
            std::string par, name;
            split(f.first, par, name);
            dmod[par].push_back(Ent{name, slotsFor(name), true, nextCluster});
            nextCluster += uint32_t(f.second.size() / kCluster + 1);
        }
    }

    // ---- IStorage
    Handle open(const char* path, OpenMode mode) override {
        Guard g(*this);
        std::string par, name;
        split(path, par, name);
        const bool parOk = walk(par);
        const bool had = files.count(path) != 0;
        const Handle h = FakeStorage::open(path, mode);
        if (h < 0) {
            if (parOk) find(par, name);
            return h;
        }
        H st;
        st.path = path;
        if (mode == kRead) {
            find(par, name);
            Ent* e = ent(path);
            st.cluster = e ? e->cluster : 0;
            st.size = uint32_t(files[path].size());
        } else {
            const int idx = find(par, name);
            if (idx >= 0 && mode == kWrite) {
                Ent* e = ent(path);
                if (e && had) freeChain(e->cluster, 1);
            }
            if (idx < 0) registerEntry(par, name);
            Ent* e = ent(path);
            if (e && !e->cluster) e->cluster = nextCluster;
            st.cluster = e ? e->cluster : 0;
            st.size = mode == kWrite ? 0 : uint32_t(files[path].size());
        }
        hs[h] = st;
        return h;
    }
    void clusterStep(H& h, uint32_t p, bool alloc) {
        const int64_t cl = p / kCluster;
        if (cl == h.curCl) return;
        if (alloc && uint64_t(cl) * kCluster >= h.size) {
            fatAccess(h.cluster + uint32_t(cl), true);   // create_chain
            if (uint32_t(h.cluster + cl + 1) > nextCluster) nextCluster = h.cluster + uint32_t(cl) + 1;
        } else if (cl > 0 && !fastSeek) {
            fatAccess(h.cluster + uint32_t(cl) - 1);   // get_fat
        }
        h.curCl = cl;
    }
    void ioCost(H& h, uint32_t n, bool write) {
        uint32_t p = h.pos;
        const uint32_t end = write ? p + n : std::min(p + n, h.size);
        while (p < end) {
            clusterStep(h, p, write);
            const uint32_t sec = p / 512, off = p % 512;
            if (off == 0 && end - p >= 512) {
                const uint32_t left = kSecPerCl - (sec % kSecPerCl);
                const uint32_t cnt = std::min((end - p) / 512, left);
                if (write) wr(cnt);
                else rd(cnt);
                p += cnt * 512;
            } else {
                if (h.bufSec != int64_t(sec)) {
                    if (h.bufDirty) wr(1);
                    h.bufDirty = false;
                    if (!write || uint64_t(sec) * 512 < h.size) rd(1);
                    h.bufSec = sec;
                }
                if (write) h.bufDirty = true;
                p = std::min(end, (sec + 1) * 512);
            }
        }
        h.pos = p;
        if (write) {
            h.written = true;
            if (p > h.size) h.size = p;
        }
    }
    int32_t read(Handle h, void* dst, uint32_t len) override {
        Guard g(*this);
        const int32_t r = FakeStorage::read(h, dst, len);
        auto it = hs.find(h);
        if (r > 0 && it != hs.end()) ioCost(it->second, uint32_t(r), false);
        else if (r == 0 && it != hs.end()) clock->us += 1;
        return r;
    }
    int32_t write(Handle h, const void* src, uint32_t len) override {
        Guard g(*this);
        const int32_t r = FakeStorage::write(h, src, len);
        auto it = hs.find(h);
        if (r > 0 && it != hs.end()) ioCost(it->second, uint32_t(r), true);
        return r;
    }
    bool seek(Handle h, uint32_t pos) override {
        Guard g(*this);
        auto it = hs.find(h);
        if (it != hs.end()) {
            H& s = it->second;
            const int64_t cl = pos / kCluster;
            if (!fastSeek && cl > 0) fatAccess(s.cluster);   // chain walk from the start
            s.curCl = pos == s.pos ? s.curCl : (cl == s.curCl ? cl : -1);
            if (s.curCl < 0) s.curCl = cl;
            s.pos = pos;
        }
        return FakeStorage::seek(h, pos);
    }
    int32_t size(Handle h) override { return FakeStorage::size(h); }
    void close(Handle h) override {
        Guard g(*this);
        auto it = hs.find(h);
        if (it != hs.end()) {
            H& s = it->second;
            if (s.written) {
                if (s.bufDirty) wr(1);
                if (win.dirty) wr(1);
                win.dirty = false;
                std::string par, name;
                split(s.path, par, name);
                // Directory entry update (sync) + FSINFO.
                Win w;
                w.kind = 1;
                w.dir = par;
                w.sec = 0;
                winTo(w);
                wr(1);
                wr(1);
            }
            hs.erase(it);
        }
        FakeStorage::close(h);
    }
    bool exists(const char* path) override {
        Guard g(*this);
        walk(path);
        return FakeStorage::exists(path);
    }
    bool mkdir(const char* path) override {
        Guard g(*this);
        std::string par, name;
        split(path, par, name);
        const bool had = FakeStorage::exists(path);
        if (!walk(par)) return FakeStorage::mkdir(path);
        if (find(par, name) >= 0) return FakeStorage::mkdir(path);
        const bool ok = FakeStorage::mkdir(path);
        if (ok && !had) {
            registerEntry(par, name);
            ent(path)->cluster = nextCluster++;
            dmod[path];
            fatAccess(nextCluster, true);
            wr(kSecPerCl);   // zero-filled directory cluster
            if (win.dirty) wr(1);
            win.dirty = false;
        }
        return ok;
    }
    bool remove(const char* path) override {
        Guard g(*this);
        const bool there = walk(path);
        const bool ok = FakeStorage::remove(path);
        if (there && ok) {
            Ent* e = ent(path);
            const auto f = files.find(path);
            (void)f;
            if (e) {
                freeChain(e->cluster, kCluster);
                e->alive = false;
            }
            dmod.erase(path);
            win.dirty = true;
            if (win.dirty) wr(1);
            win.dirty = false;
        }
        return ok;
    }
    bool rename(const char* from, const char* to) override {
        Guard g(*this);
        const bool there = walk(from);
        std::string par, name;
        split(to, par, name);
        walk(par);
        find(par, name);
        uint32_t cl = 0;
        if (Ent* e = ent(from)) cl = e->cluster;
        const bool ok = FakeStorage::rename(from, to);
        if (ok && there) {
            registerEntry(par, name);
            if (Ent* e = ent(to)) e->cluster = cl;
            if (Ent* e = ent(from)) e->alive = false;
            wr(1);
            if (win.dirty) wr(1);
            win.dirty = false;
        }
        return ok;
    }
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        Guard g(*this);
        if (walk(path)) {
            const uint32_t all = totalSlots(path);
            dirSectors(path, 0, all ? all - 1 : 0);
        }
        cpuOn = false;   // the callbacks run App code; counted below
        const int32_t r = FakeStorage::listDir(path, fn, ctx);
        cpuOn = true;
        return r;
    }
};

std::vector<uint8_t> readHostFile(const sfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(const void* d, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    template <class T>
    void val(const T& v) {
        add(&v, sizeof(v));
    }
};

uint64_t stateHash(App& a, SimCard& c) {
    Fnv f;
    static uint8_t buf[pamd::kFileSize > mtp::kFileSize ? (pamd::kFileSize > mt::kFileSize ? pamd::kFileSize : mt::kFileSize)
                                                         : mtp::kFileSize];
    encodeProject(a.project, buf);
    f.add(buf, mt::kFileSize);
    for (int n = 1; n <= kPatternCount; ++n) {
        encodePattern(a.stored(n), buf);
        f.add(buf, mtp::kFileSize);
        f.val(a.patternPresent(n));
    }
    encodePatternNames(a.names, buf);
    f.add(buf, pamd::kFileSize);
    for (int i = 0; i < kSampleInstrumentCount; ++i) {
        uint8_t hdr[pti::kHeaderBlockSize];
        encodePtiHeader(a.instruments[i], hdr);
        f.add(hdr, sizeof(hdr));
        f.val(a.instrPresent[i]);
        const uint32_t fr = a.bank.frames(i);
        const bool st = a.bank.stereo(i);
        f.val(fr);
        f.val(st);
        if (fr) f.add(a.bank.left(i), size_t(fr) * (st ? 2 : 1) * 2);
    }
    for (auto& kv : c.files)
        if (kv.first.compare(0, 10, "Workspace/") == 0) {
            f.add(kv.first.data(), kv.first.size());
            f.add(kv.second.data(), kv.second.size());
        }
    return f.h;
}

struct RunStats {
    int passes = 0;
    double totalUs = 0, maxPassUs = 0;
    uint64_t cmds = 0;
};

RunStats runJob(App& a, SimCard& c, SimClock& clk) {
    RunStats r;
    const uint64_t cmd0 = c.cmds;
    while (a.jobBusy() && r.passes < 1000000) {
        const double t0 = clk.us;
        c.cpuOn = true;
        c.leave();
        a.serviceProjectJob();
        c.enter();
        c.cpuOn = false;
        const double dt = clk.us - t0;
        r.totalUs += dt;
        if (dt > r.maxPassUs) r.maxPassUs = dt;
        ++r.passes;
        clk.us += 16000;   // rest of the main loop (UI, input) before the next pass
    }
    r.cmds = c.cmds - cmd0;
    return r;
}

}  // namespace

TEST(load_perf_over_backup_projects) {
    const sfs::path projects = sfs::path(backupRoot()) / "Projects";
    std::error_code ec;
    if (!sfs::is_directory(projects, ec)) {
        std::printf("  SKIP: %s absent\n", projects.string().c_str());
        return;
    }
    if (const char* c = std::getenv("OT_PERF_CPU")) kCpuScale = std::atof(c);
    const char* fsEnv = std::getenv("OT_PERF_FASTSEEK");
    const bool fastSeek = fsEnv && fsEnv[0] == '1';
    std::vector<sfs::path> dirs;
    for (auto it = sfs::recursive_directory_iterator(projects, ec); it != sfs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename() == "project.mt") dirs.push_back(it->path().parent_path());
    std::sort(dirs.begin(), dirs.end());
    RunStats sumOpen, sumBoot;
    double worstOpen = 0, worstBoot = 0, worstOpenTotal = 0;
    std::string worstName;
    uint64_t combined = 1469598103934665603ull;
    int n = 0;
    std::printf("  model: cpu x%.1f, %s; open = Projects -> Workspace + load, boot = Workspace reload\n",
                kCpuScale, fastSeek ? "fast seek (CLMT)" : "no fast seek");
    for (const sfs::path& dir : dirs) {
        const std::string rel = "Projects/" + sfs::relative(dir, projects).generic_string();
        const std::string name = dir.filename().string();
        if (name.size() > mt::kNameMax) continue;
        Harness h(false, 0xB40000 / 2);
        SimCard card;
        SimClock clk;
        card.clock = &clk;
        card.fastSeek = fastSeek;
        HalRefs hr = h.app->hal();
        hr.storage = &card;
        hr.clock = &clk;
        App& a = *h.app;
        CHECK(a.init(hr, a.mem()));
        for (size_t p = rel.find('/'); p != std::string::npos; p = rel.find('/', p + 1)) card.dirs.insert(rel.substr(0, p));
        card.dirs.insert(rel);
        for (auto it = sfs::recursive_directory_iterator(dir, ec); it != sfs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string r = rel + "/" + sfs::relative(it->path(), dir).generic_string();
            if (it->is_directory()) card.dirs.insert(r);
            else card.files[r] = readHostFile(it->path());
        }
        card.buildModel();
        CHECK(a.boot());
        const std::string folder = rel.substr(0, rel.size() - name.size() - 1);
        if (!projectFitsMemory(a, folder.c_str(), name.c_str())) continue;
        CHECK(a.startOpenProject(folder.c_str(), name.c_str()));
        const RunStats ro = runJob(a, card, clk);
        CHECK(a.lastJobOk());
        CHECK(a.lastJobErrors() == 0);
        const uint64_t hOpen = stateHash(a, card);
        const JobProfile po = jobProfile();
        CHECK(a.startLoadWorkspace());
        const RunStats rb = runJob(a, card, clk);
        CHECK(a.lastJobOk());
        const uint64_t hBoot = stateHash(a, card);
        const JobProfile pb = jobProfile();
        if (std::getenv("OT_PERF_PROFILE"))
            for (const JobProfile* q : {&po, &pb})
                std::printf("    profile op %d: %u passes %u units, max pass %.1f ms (stage %u.%u), max unit %.1f ms (stage %u), open %.0f read %.0f write %.0f "
                            "dir %.0f decode %.0f commit %.0f ms, %u opens %u dir ops\n",
                            q->op, q->passes, q->units, q->maxPassUs / 1e3, q->maxPassStage, q->maxPassPhase, q->maxUnitUs / 1e3, q->maxUnitStage, q->openUs / 1e3,
                            q->readUs / 1e3, q->writeUs / 1e3, q->dirUs / 1e3, q->decodeUs / 1e3, q->commitUs / 1e3,
                            q->opens, q->dirOps);
        std::printf("  %-38s open %5d passes %7.2f s max %6.1f ms %6llu cmds | boot %5d passes %6.2f s max %6.1f ms | %016llx %016llx\n",
                    name.c_str(), ro.passes, ro.totalUs / 1e6, ro.maxPassUs / 1e3, (unsigned long long)ro.cmds, rb.passes,
                    rb.totalUs / 1e6, rb.maxPassUs / 1e3, (unsigned long long)hOpen, (unsigned long long)hBoot);
        sumOpen.passes += ro.passes;
        sumOpen.totalUs += ro.totalUs;
        sumOpen.cmds += ro.cmds;
        sumBoot.passes += rb.passes;
        sumBoot.totalUs += rb.totalUs;
        sumBoot.cmds += rb.cmds;
        worstOpen = std::max(worstOpen, ro.maxPassUs);
        worstBoot = std::max(worstBoot, rb.maxPassUs);
        if (ro.totalUs > worstOpenTotal) {
            worstOpenTotal = ro.totalUs;
            worstName = name;
        }
        combined = (combined ^ hOpen ^ (hBoot << 1)) * 1099511628211ull;
        ++n;
    }
    std::printf("  %d projects: open %.1f s job time (%d passes, %llu cmds, max pass %.1f ms; slowest %s %.2f s), "
                "boot %.1f s (max pass %.1f ms); combined hash %016llx\n",
                n, sumOpen.totalUs / 1e6, sumOpen.passes, (unsigned long long)sumOpen.cmds, worstOpen / 1e3,
                worstName.c_str(), worstOpenTotal / 1e6, sumBoot.totalUs / 1e6, worstBoot / 1e3,
                (unsigned long long)combined);
    CHECK(n > 0);
}
