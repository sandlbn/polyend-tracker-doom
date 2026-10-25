#include "games/session.h"

#include <cstdio>
#include <cstring>

namespace ot {
namespace games {

namespace {
constexpr uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }
constexpr uint32_t kCartBytes = Nes::kChrRamBytes + Nes::kPrgRamBytes + Nes::kVramExtraBytes;
constexpr uint32_t kSpriteLineBytes = align16(Nes::kHeight * sizeof(uint64_t));
constexpr uint32_t kFrameAudioBytes = align16(Nes::kAudioCap * sizeof(int16_t));
constexpr uint32_t kRingBytes = Session::kRingSamples * sizeof(int16_t);
constexpr uint32_t kFbBytes = IDisplay::kGameFrameBytes;
// Audio latency target: the producer drops samples above this fill.
constexpr uint32_t kRingHigh = 3 * 735 + 128;
constexpr uint32_t kRomChunk = 0x4000;
static_assert((Session::kRingSamples & (Session::kRingSamples - 1)) == 0, "ring size is a power of two");
static_assert(kRingHigh + Nes::kAudioCap < Session::kRingSamples, "ring headroom");
}  // namespace

Session& session() {
    static Session s;
    return s;
}

uint32_t Session::arenaBytes(uint32_t romBytes) {
    return align16(romBytes) + kCartBytes + kSpriteLineBytes + kFrameAudioBytes + kRingBytes + kFbBytes;
}

LoadStatus Session::load(IStorage& fs, const char* path, uint8_t* arena, uint32_t arenaCap) {
    unload();
    err_[0] = 0;
    auto fail = [&](LoadStatus st, const char* text) {
        std::snprintf(err_, sizeof(err_), "%s", text);
        return st;
    };
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return fail(LoadStatus::OpenFailed, "Cannot open the ROM file");
    const int32_t size = fs.size(h);
    if (size <= 0) {
        fs.close(h);
        return fail(LoadStatus::ReadFailed, "Cannot read the ROM file");
    }
    if (uint32_t(size) > kMaxRomBytes) {   // spec 14 §1.6: checked before reading
        fs.close(h);
        return fail(LoadStatus::TooLarge, "ROM file too large");
    }
    const uint32_t need = arenaBytes(uint32_t(size));
    if (!arena || (reinterpret_cast<uintptr_t>(arena) & 15u) || arenaCap < need) {
        fs.close(h);
        return fail(LoadStatus::NoMemory, "Not enough memory for this game");
    }
    uint32_t got = 0;
    while (got < uint32_t(size)) {
        const uint32_t want = uint32_t(size) - got < kRomChunk ? uint32_t(size) - got : kRomChunk;
        const int32_t r = fs.read(h, arena + got, want);
        if (r <= 0) break;
        got += uint32_t(r);
    }
    fs.close(h);
    if (got != uint32_t(size)) return fail(LoadStatus::ReadFailed, "Cannot read the ROM file");

    uint8_t* p = arena + align16(uint32_t(size));
    NesMemory m;
    m.chrRam = p;
    p += Nes::kChrRamBytes;
    m.prgRam = p;
    p += Nes::kPrgRamBytes;
    m.vramExtra = p;
    p += Nes::kVramExtraBytes;
    fingerprintBytes_ = uint32_t(p - arena);
    m.spriteLines = reinterpret_cast<uint64_t*>(p);
    p += kSpriteLineBytes;
    m.audio = reinterpret_cast<int16_t*>(p);
    frameAudio_ = m.audio;
    p += kFrameAudioBytes;
    ring_ = reinterpret_cast<int16_t*>(p);
    p += kRingBytes;
    fb_ = p;
    const NesError e = nes_.load(arena, uint32_t(size), m);
    if (e != NesError::Ok) {
        if (e == NesError::Mapper) std::snprintf(err_, sizeof(err_), "Mapper %d not supported", nes_.mapper());
        else std::snprintf(err_, sizeof(err_), "%s", nesErrorText(e));
        fb_ = nullptr;
        ring_ = nullptr;
        frameAudio_ = nullptr;
        return LoadStatus::BadRom;
    }
    uint8_t lut[64];
    Nes::rgb332Lut(lut);   // spec 14 §3.1 RGB332
    nes_.setPalette(lut);
    nes_.setSampleRate(IAudioOut::kSampleRate);
    std::memset(fb_, 0, kFbBytes);
    std::memset(ring_, 0, kRingBytes);
    wr_ = rd_ = 0;
    last_ = 0;
    arena_ = arena;
    pad_ = 0;
    started_ = false;
    accUs_ = 0;
    framesRun_ = framesDrawn_ = framesDropped_ = uploadBytes_ = 0;
    nes_.markAllDirty();
    loaded_ = true;
    return LoadStatus::Ok;
}

void Session::unload() {
    audioActive_ = false;
    loaded_ = false;
    nes_.unload();
    arena_ = nullptr;
    fb_ = nullptr;
    ring_ = nullptr;
    frameAudio_ = nullptr;
    wr_ = rd_ = 0;
}

void Session::resync(uint32_t nowUs) {
    lastUs_ = nowUs;
    accUs_ = 0;
    started_ = true;
}

bool Session::service(uint32_t nowUs) {
    if (!loaded_ || nes_.jammed()) return false;
    if (!started_) resync(nowUs);
    const int32_t dt = int32_t(nowUs - lastUs_);
    lastUs_ = nowUs;
    if (dt > 0) accUs_ += dt;
    const int32_t frameUs = int32_t(Nes::kFrameUs);
    if (accUs_ < frameUs) return false;
    int due = int(accUs_ / frameUs);
    if (due > kMaxFramesPerPass) {
        // Too far behind (slow pass, card access): drop the backlog.
        framesDropped_ += uint32_t(due - 1);
        accUs_ = frameUs + accUs_ % frameUs;
        due = 1;
    }
    bool drawn = false;
    for (int i = 0; i < due; ++i) {
        // spec 14 §3.3: frames run late are emulated without drawing; only the
        // last due frame of this pass is drawn.
        const bool draw = i == due - 1;
        nes_.setPad(pad_);
        if (!nes_.runFrame(draw ? fb_ : nullptr)) break;
        ++framesRun_;
        pushAudio();
        accUs_ -= frameUs;
        if (draw) {
            ++framesDrawn_;
            drawn = true;
        }
    }
    return drawn;
}

void Session::pushAudio() {
    // Single producer (main loop). Above the latency target, new samples are
    // dropped so the ISR never sees a stale ring.
    const uint32_t n = nes_.audioCount();
    const int16_t* src = frameAudio_;
    if (!src || !ring_) return;
    uint32_t w = wr_;
    const uint32_t fill = w - rd_;
    uint32_t room = fill < kRingHigh ? kRingHigh - fill : 0;
    const uint32_t take = n < room ? n : room;
    for (uint32_t i = 0; i < take; ++i) ring_[(w + i) & (kRingSamples - 1)] = src[i];
    wr_ = w + take;
}

void Session::fillAudio(int16_t* out, uint32_t frames, void* ctx) {
    (void)ctx;
    Session& s = session();
    const int16_t* ring = s.ring_;
    if (!s.audioActive_ || !ring) {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
        return;
    }
    uint32_t r = s.rd_;
    const uint32_t w = s.wr_;
    int16_t v = s.last_;
    for (uint32_t i = 0; i < frames; ++i) {
        if (r != w) {
            v = ring[r & (kRingSamples - 1)];
            ++r;
        } else {
            v = int16_t(v - v / 16);   // starved: fade the held sample instead of clicking
        }
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    s.last_ = v;
    s.rd_ = r;
}

void Session::setAudioActive(bool on) {
    if (on) rd_ = wr_;   // start from fresh samples
    audioActive_ = on && loaded_;
}

uint32_t Session::upload(IDisplay& d) {
    if (!loaded_ || !fb_) return 0;
    uint32_t bytes = 0;
    int y = 0;
    while (y < Nes::kHeight) {
        if (!nes_.rowDirty(y)) {
            ++y;
            continue;
        }
        int y1 = y + 1;
        while (y1 < Nes::kHeight && nes_.rowDirty(y1)) ++y1;
        const uint32_t off = uint32_t(y) * Nes::kWidth, len = uint32_t(y1 - y) * Nes::kWidth;
        if (d.writeRamG(IDisplay::kRamGGameFrame + off, fb_ + off, len)) bytes += len;
        y = y1;
    }
    nes_.clearDirty();
    uploadBytes_ += bytes;
    return bytes;
}

void Session::invalidateDisplay() { nes_.markAllDirty(); }

uint32_t Session::fingerprint() const {
    if (!arena_) return 0;
    // FNV-1a over 32-bit words (the region is 16-byte aligned and sized).
    uint32_t hsh = 2166136261u;
    for (uint32_t i = 0; i + 4 <= fingerprintBytes_; i += 4) {
        uint32_t w;
        std::memcpy(&w, arena_ + i, 4);
        hsh = (hsh ^ w) * 16777619u;
    }
    return hsh;
}

}  // namespace games
}  // namespace ot
