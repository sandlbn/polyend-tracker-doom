// Byte-level format check of a saved project (testsongs.h
// checkStockProject). Written from docs/specs/02-file-formats.md (§1, §2,
// §3, §4, §5, §6), the step FX ranges in docs/specs/03-sequencer.md §3 and,
// for the Mini mode, docs/specs/21 (§2 patch files, §3.2/§3.3 project.mt,
// §4 16-track patterns), independently of the src/core codecs: raw bytes
// only, own CRC.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hal_api.h"
#include "testsongs.h"

namespace ts {

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {   // CRC-32/ISO-HDLC (spec 02 §2)
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
uint16_t u16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t u32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}
float f32(const std::vector<uint8_t>& b, size_t o) {
    const uint32_t v = u32(b, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool readAll(ot::IStorage& fs, const std::string& path, std::vector<uint8_t>& out) {
    const ot::IStorage::Handle h = fs.open(path.c_str(), ot::IStorage::kRead);
    if (h < 0) return false;
    const int32_t n = fs.size(h);
    out.assign(n > 0 ? size_t(n) : 0, 0);
    const bool ok = n >= 0 && (n == 0 || fs.read(h, out.data(), uint32_t(n)) == n);
    fs.close(h);
    return ok;
}

struct Entry {
    std::string name;
    bool dir;
};
std::vector<Entry> list(ot::IStorage& fs, const std::string& path) {
    std::vector<Entry> v;
    fs.listDir(
        path.c_str(),
        [](const ot::IStorage::DirEntry& e, void* c) {
            static_cast<std::vector<Entry>*>(c)->push_back({e.name, e.isDir});
            return true;
        },
        &v);
    return v;
}

// Highest stored value the stock engine accepts per step FX id (spec 03 §3
// table; where spec 02 §4.3 is narrower, the narrower range).
struct FxRange {
    uint8_t lo, hi;
};
const FxRange kFx[43] = {
    {0, 255}, {0, 0},   {0, 100}, {0, 47},  {0, 100}, {0, 100}, {0, 47},  {0, 100}, {0, 127}, {0, 127}, {0, 127},
    {0, 127}, {0, 127}, {1, 1},   {0, 15},  {4, 200}, {0, 255}, {25, 75}, {0, 100}, {0, 100}, {0, 100}, {0, 33},
    {0, 100}, {0, 23},  {0, 28},  {0, 47},  {0, 1},   {0, 100}, {0, 100}, {0, 100}, {0, 100}, {0, 100}, {0, 100},
    {0, 28},  {0, 198}, {0, 28},  {0, 28},  {0, 127}, {0, 100}, {4, 16},  {0, 48},  {0, 255}, {0, 255},
};

struct Checker {
    std::string out;
    int problems = 0;
    void bad(const std::string& where, const char* fmt, long a = 0, long b = 0) {
        if (++problems > 20) return;
        char m[200];
        std::snprintf(m, sizeof(m), fmt, a, b);
        out += where + ": " + m + "\n";
    }
};

void checkProject(Checker& c, const std::vector<uint8_t>& b, const std::string& name, std::vector<int>& songPats, bool mini) {
    const std::string w = "project.mt";
    if (b.size() != 0x914) return c.bad(w, "size %ld, want 2324", long(b.size()));
    if (b[0] != 'M' || b[1] != 'T') c.bad(w, "magic");
    if (u16(b, 2) != 0) c.bad(w, "file type %ld (loader requires 0)", u16(b, 2));
    for (int i = 0; i < 4; ++i)
        if (b[8 + i] != 17) c.bad(w, "structure version byte %ld = %ld", i, b[8 + i]);
    if (u16(b, 0x0C) != 0x914) c.bad(w, "size field %ld", u16(b, 0x0C));
    if (crc32(b.data(), 0x910) != u32(b, 0x910)) c.bad(w, "CRC mismatch");
    if (b[0x830] || b[0x90E]) c.bad(w, "name / folder not NUL-terminated");
    if (std::strncmp(reinterpret_cast<const char*>(&b[0x810]), name.c_str(), 32) != 0) c.bad(w, "name differs from folder");
    // Song (§3.2): pattern numbers until the first 0.
    for (int i = 0; i < 255 && b[0x10 + i]; ++i) songPats.push_back(b[0x10 + i]);
    if (songPats.empty()) c.bad(w, "empty song");
    // Load sanitiser (§3.3 "Validation on load"): nothing may be changed.
    if (int8_t(b[0x110]) < 0) c.bad(w, "0x110 < 0");
    if (b[0x111] > 63) c.bad(w, "0x111 = %ld > 63", b[0x111]);
    if (b[0x113] > 49) c.bad(w, "0x113 = %ld > 49", b[0x113]);
    if (b[0x114] < 1) c.bad(w, "current pattern 0");
    for (int i = 0; i < 8; ++i)
        if (b[0x12C + i] > 1) c.bad(w, "0x12C[%ld] = %ld > 1", i, b[0x12C + i]);
    if (b[0x134] > (mini ? 2 : 3)) c.bad(w, "0x134 = %ld out of range", b[0x134]);
    for (int i = 0; i < 8; ++i)
        if (u16(b, 0x142 + 2 * i) < 1 || u16(b, 0x142 + 2 * i) > 255) c.bad(w, "0x142[%ld] = %ld", i, u16(b, 0x142 + 2 * i));
    for (int i = 0; i < 12; ++i)
        if (b[0x152 + i] >= 22) c.bad(w, "0x152[%ld] = %ld (perf FX id)", i, b[0x152 + i]);
    for (int i = 0; i < 12; ++i)
        for (int r = 0; r < 4; ++r) {
            const int v = int16_t(u16(b, 0x15E + 8 * i + 2 * r));
            if (r == 0 ? v != 0 : (v < -255 || v > 255)) c.bad(w, "0x15E[%ld] = %ld", i * 4 + r, v);
        }
    const float tempo = f32(b, 0x1C0);
    if (!(tempo > 0.0f && tempo <= 1000.0f)) c.bad(w, "tempo out of range");
    if (u16(b, 0x1C4) > 255) c.bad(w, "0x1C4 > 255");
    for (int i = 0; i < 12; ++i)
        if (b[0x2F6 + i] > 3) c.bad(w, "0x2F6[%ld] = %ld > 3", i, b[0x2F6 + i]);
    for (int r = 0; r < 16; ++r) {
        const size_t o = 0x4FD + 16 * size_t(r);
        if (b[o] > 16) c.bad(w, "MIDI record %ld channel %ld", r, b[o]);
        if (b[o + 1] & 0x80) c.bad(w, "MIDI record %ld velocity", r);
        for (int k = 0; k < 6; ++k)
            if (b[o + 2 + k] > 130) c.bad(w, "MIDI record %ld CC %ld > 130", r, k);
        for (int k = 8; k < 16; ++k)
            if (b[o + k]) c.bad(w, "MIDI record %ld byte %ld not 0", r, k);
    }
    if (u16(b, 0x5FE) < 100 || u16(b, 0x5FE) > 20000) c.bad(w, "0x5FE out of range");
    if (b[0x600] > 1 || b[0x601] > 4 || b[0x602] > 1) c.bad(w, "0x600..0x602 out of range");
    for (int i = 0; i < 8; ++i)
        if (b[0x742 + i] > 1) c.bad(w, "0x742[%ld] > 1", i);
    if (b[0x80C] > 100) c.bad(w, "0x80C > 100");
    // Short names of tracks 9-16 (0x603, 8 bytes each): NUL-terminated.
    for (int i = 0; i < 8; ++i)
        if (!std::memchr(&b[0x603 + 8 * size_t(i)], 0, 8)) c.bad(w, "name of track %ld not NUL-terminated", 9 + i);
    if (!mini) {
        // Stock 1.9.2 initialises 0x74C..0x80B to zero and never reads it.
        for (size_t o = 0x74C; o < 0x80C; ++o)
            if (b[o]) return c.bad(w, "0x%lX not 0 (stock reserved area)", long(o));
        return;
    }
    // Tracker Mini synth-slot records (spec 21 §3.3): any failed check makes
    // the Mini replace the whole record, so none may fail.
    for (int s = 0; s < 3; ++s) {
        const size_t r = 0x74C + 0x40 * size_t(s);
        const float gain = f32(b, r), pan = f32(b, r + 0x0C), rev = f32(b, r + 0x30), del = f32(b, r + 0x34);
        if (!(gain >= -96.0f && gain <= 12.0f)) c.bad(w, "synth slot %ld gain out of range", s + 1);
        if (u32(b, r + 4) > 8) c.bad(w, "synth slot %ld polyphony %ld", s + 1, long(u32(b, r + 4)));
        if (u32(b, r + 8) > 4) c.bad(w, "synth slot %ld model %ld", s + 1, long(u32(b, r + 8)));
        if (!(pan >= -100.0f && pan <= 100.0f)) c.bad(w, "synth slot %ld pan out of range", s + 1);
        if (!(rev >= 0.0f && rev <= 100.0f) || !(del >= 0.0f && del <= 100.0f)) c.bad(w, "synth slot %ld sends out of range", s + 1);
        if (!std::memchr(&b[r + 0x10], 0, 32)) c.bad(w, "synth slot %ld patch name not NUL-terminated", s + 1);
        for (int m = 0; m < 6; ++m)
            if (b[r + 0x38 + size_t(m)] & 0x80) c.bad(w, "synth slot %ld macro %ld > 127", s + 1, m + 1);
    }
}

void checkPattern(Checker& c, const std::vector<uint8_t>& b, const std::string& w, bool mini) {
    // Stock: 12 tracks, 0x242C bytes. Tracker Mini: 16 tracks, 0x3030 bytes,
    // instruments 64..66 = Synth 1..3 (spec 21 §4.1).
    const int tracks = mini ? 16 : 12;
    const size_t size = 0x1C + 0x301 * size_t(tracks) + 4, crcOff = size - 4;
    if (b.size() != size) return c.bad(w, "size %ld, want %ld", long(b.size()), long(size));
    if (u16(b, 2) != 2) c.bad(w, "type %ld", u16(b, 2));
    for (int i = 0; i < 4; ++i)
        if (b[8 + i] != 5) c.bad(w, "structure version %ld", b[8 + i]);
    if (u16(b, 0x0C) != size) c.bad(w, "size field %ld", u16(b, 0x0C));
    if (crc32(b.data(), crcOff) != u32(b, crcOff)) c.bad(w, "CRC mismatch");
    const int maxInstr = mini ? 66 : 63;
    for (int t = 0; t < tracks; ++t) {
        const size_t to = 0x1C + size_t(t) * 0x301;
        if (b[to] > 127) c.bad(w, "track %ld last step %ld", t + 1, b[to]);
        for (int s = 0; s < 128; ++s) {
            const size_t o = to + 1 + 6 * size_t(s);
            const int note = int8_t(b[o]);
            if (note < -4) c.bad(w, "track %ld step %ld: undefined note sentinel", t + 1, s + 1);
            if (b[o + 1] > maxInstr) c.bad(w, "track %ld step %ld: instrument out of range", t + 1, s + 1);
            for (int k = 0; k < 2; ++k) {
                const uint8_t type = b[o + 2 + 2 * k], val = b[o + 3 + 2 * k];
                if (type >= 43) c.bad(w, "track %ld step %ld: FX id >= 43", t + 1, s + 1);
                else if (type && (val < kFx[type].lo || val > kFx[type].hi))
                    c.bad(w, "track %ld: FX %ld value out of range", t + 1, type);
            }
        }
    }
}

void checkNames(Checker& c, const std::vector<uint8_t>& b) {
    const std::string w = "patternsMetadata";
    if (b.size() != 0x3210) return c.bad(w, "size %ld", long(b.size()));
    if (std::memcmp(b.data(), "PAMD", 4) != 0) c.bad(w, "magic");
    if (u16(b, 4) != 1 || u32(b, 8) != 0x3210) c.bad(w, "header");
    for (int r = 1; r < 256; ++r)
        if (std::memchr(&b[0x10 + 50 * size_t(r)], 0, 31) == nullptr) c.bad(w, "record %ld not NUL-terminated", r);
}

void checkInstrument(Checker& c, const std::vector<uint8_t>& b, const std::string& file) {
    const std::string w = "instruments/" + file;
    if (b.size() < 0x188) return c.bad(w, "short file");
    if (b[0] != 'T' || b[1] != 'I') c.bad(w, "magic");
    if (u16(b, 2) != 1) c.bad(w, "type %ld (required 1)", u16(b, 2));
    for (int i = 0; i < 4; ++i)
        if (b[8 + i] != 9) c.bad(w, "structure version %ld", b[8 + i]);
    if (u16(b, 0x0C) != 0x174) c.bad(w, "body size %ld", u16(b, 0x0C));
    if (crc32(b.data(), 0x184) != u32(b, 0x184)) c.bad(w, "CRC mismatch");
    // "<N> <name>.pti", N = 1..48 (spec 02 §1), name = body name (§6.2 0x05).
    int n = 0;
    char nm[64] = {};
    if (std::sscanf(file.c_str(), "%d %63[^\n]", &n, nm) != 2 || n < 1 || n > 48) c.bad(w, "file name");
    const char* body = reinterpret_cast<const char*>(&b[0x10 + 0x05]);
    if (!std::memchr(body, 0, 32)) c.bad(w, "name not NUL-terminated");
    else if (std::string(body) + ".pti" != nm) c.bad(w, "file name does not match the instrument name");
    if (b[0x10 + 0x04] != 0) c.bad(w, "sample type not wave file");
    const uint32_t len = u32(b, 0x10 + 0x2C);
    const size_t payload = b.size() - 0x188;
    if (len == 0 || (payload != size_t(len) * 2 && payload != size_t(len) * 4))
        c.bad(w, "payload %ld bytes for %ld frames (neither mono nor stereo)", long(payload), long(len));
    const uint8_t mode = b[0x10 + 0x3C];
    if (mode > 7) c.bad(w, "play mode %ld", mode);
    const uint16_t st = u16(b, 0x10 + 0x3E), l1 = u16(b, 0x10 + 0x40), l2 = u16(b, 0x10 + 0x42), en = u16(b, 0x10 + 0x44);
    if (!(st <= l1 && l1 <= l2 && l2 <= en)) c.bad(w, "points not ordered start <= loop1 <= loop2 <= end");
    if (b[0x10 + 0x100] > 100) c.bad(w, "volume > 100");
    const int16_t pan = int16_t(u16(b, 0x10 + 0x104));
    if (pan < 0 || pan > 100) c.bad(w, "panning %ld", pan);
    if (b[0x10 + 0x106] > 100 || b[0x10 + 0x170] > 100 || b[0x10 + 0x171] > 100) c.bad(w, "send / overdrive > 100");
    if (b[0x10 + 0x168] > 48) c.bad(w, "slice count > 48");
    const uint8_t bits = b[0x10 + 0x172];
    if (bits < 4 || bits > 16) c.bad(w, "bit depth %ld", bits);
    const int tune = int8_t(b[0x10 + 0xFE]), fine = int8_t(b[0x10 + 0xFF]);
    if (tune < -24 || tune > 24 || fine < -100 || fine > 100) c.bad(w, "tune / finetune out of range");
}

}  // namespace

std::string checkStockProject(ot::IStorage& fs, const char* projectDir, bool mini) {
    Checker c;
    const std::string dir(projectDir);
    const std::string name = dir.substr(dir.rfind('/') + 1);
    if (name.size() > 32) c.bad(dir, "project name longer than 32 characters");
    std::vector<uint8_t> b;
    std::vector<int> song;
    if (!readAll(fs, dir + "/project.mt", b)) return "project.mt missing\n";
    checkProject(c, b, name, song, mini);
    for (int p : song) {
        char f[64];
        std::snprintf(f, sizeof(f), "/patterns/pattern_%02d.mtp", p);
        if (!fs.exists((dir + f).c_str())) c.bad("song", "pattern %ld has no file", p);
    }
    for (const Entry& e : list(fs, dir + "/patterns")) {
        if (e.dir) {
            c.bad("patterns/" + e.name, "unexpected folder");
            continue;
        }
        if (!readAll(fs, dir + "/patterns/" + e.name, b)) {
            c.bad(e.name, "unreadable");
            continue;
        }
        if (e.name == "patternsMetadata") {
            checkNames(c, b);
            continue;
        }
        int n = 0;
        if (std::sscanf(e.name.c_str(), "pattern_%d.mtp", &n) != 1 || n < 1 || n > 255) {
            c.bad("patterns/" + e.name, "not a stock pattern file name");
            continue;
        }
        checkPattern(c, b, "patterns/" + e.name, mini);
    }
    int instruments = 0;
    for (const Entry& e : list(fs, dir + "/instruments")) {
        if (e.dir || !readAll(fs, dir + "/instruments/" + e.name, b)) {
            c.bad("instruments/" + e.name, "unreadable");
            continue;
        }
        ++instruments;
        checkInstrument(c, b, e.name);
    }
    if (instruments > 48) c.bad("instruments", "%ld files (max 48)", instruments);
    int patches = 0;
    for (const Entry& e : list(fs, dir)) {
        if (e.name == "project.mt" || e.name == "patterns" || e.name == "instruments") continue;
        // Tracker Mini synth slot patches 0.patch .. 2.patch (spec 21 §2):
        // protobuf, no header; a non-empty file whose first byte is the
        // engine_id field (field 1, varint: 0x08).
        if (mini && !e.dir && e.name.size() == 7 && e.name[0] >= '0' && e.name[0] <= '2' && e.name.substr(1) == ".patch") {
            if (!readAll(fs, dir + "/" + e.name, b) || b.empty() || b.size() > 8192 || b[0] != 0x08)
                c.bad(e.name, "not a synth patch");
            ++patches;
            continue;
        }
        c.bad(e.name, mini ? "not part of a Tracker Mini project" : "not part of a stock project");
    }
    if (mini && patches != 3) c.bad(dir, "%ld synth patch files, want 3", patches);
    if (c.problems > 20) c.out += "... and more\n";
    return c.out;
}

}  // namespace ts
