// Impulse Tracker IT import (spec 11 §1.3).
//
// Deviations from stock 1.9.2 (spec 11 defects; reimpl/README.md lists them):
// - the IMPM magic is checked; sample-mode files (no instruments) import
//   their samples as instruments (stock reads the header as an instrument);
// - 8-bit samples use the same 44100 Hz reference as 16-bit (stock 22050:
//   one octave high); unsigned, delta, stereo and compressed (IT 2.14 /
//   2.15) samples are decoded (stock copies the raw bytes);
// - ping-pong without the loop flag is one-shot; a sustain loop is used
//   when there is no normal loop;
// - pattern note / instrument memory is per channel (stock shares one
//   value between channels); a note without an instrument uses the
//   channel's instrument; instrument 48 is usable; note off / cut / fade
//   become OFF / CUT / FADE (stock saturates them to note 127);
// - "+++" orders are skipped (stock: pattern 255); the song ends at the
//   first "---"; a referenced empty pattern becomes 64 empty rows;
// - the project tempo is 6 * IT / IS (stock ignores the initial speed);
// - effects (stock drops all): Axx / Txx -> Tempo, Cxx -> Break pattern,
//   Gxx -> Glide, Oxx -> Position, Xxx -> Panning (FX slot 0; the volume
//   column keeps FX slot 1 as stock).
#include "formats/it_import.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/bytes.h"
#include "model/fx.h"

namespace ot {

namespace itfmt {

uint8_t ByteReader::next() {
    if (i >= n) {
        base += n;
        i = 0;
        uint32_t got = 0;
        if (!modfmt::readAt(*fs, h, base, buf, cap, &got)) err = true;
        n = got;
        if (!n) {
            ++consumed;
            return 0;
        }
    }
    ++consumed;
    return buf[i++];
}

int32_t ItDecompressor::decodeBlock(ByteReader& rd, int16_t* out) {
    if (!left) return 0;
    const uint32_t blockLen = uint32_t(rd.next()) | (uint32_t(rd.next()) << 8);
    const uint32_t blockEnd = rd.tell() + blockLen;
    uint32_t bytesLeft = blockLen;
    uint32_t bitbuf = 0;
    int bits = 0;
    auto readBits = [&](int w) -> uint32_t {
        while (bits < w) {
            uint32_t b = 0;
            if (bytesLeft) {
                --bytesLeft;
                b = rd.next();
            }
            bitbuf |= b << bits;
            bits += 8;
        }
        const uint32_t v = bitbuf & ((1u << w) - 1);
        bitbuf >>= w;
        bits -= w;
        return v;
    };
    const uint32_t blockFrames = is16 ? 0x4000u : 0x8000u;
    uint32_t count = left < blockFrames ? left : blockFrames;
    const int maxW = is16 ? 17 : 9;
    int width = maxW;
    uint32_t d1 = 0, d2 = 0;
    uint32_t written = 0;
    while (written < count) {
        if (width < 1 || width > maxW) return -1;
        uint32_t v = readBits(width);
        if (width < 7) {
            // Method 1: escape value 1 << (width - 1), then the new width.
            if (v == (1u << (width - 1))) {
                const int nw = int(readBits(is16 ? 4 : 3)) + 1;
                width = nw < width ? nw : nw + 1;
                continue;
            }
        } else if (width < maxW) {
            // Method 2: a band just above the border selects the new width.
            const uint32_t border = ((is16 ? 0xFFFFu : 0xFFu) >> (maxW - width)) - (is16 ? 8u : 4u);
            if (v > border && v <= border + (is16 ? 16u : 8u)) {
                const int nw = int(v - border);
                width = nw < width ? nw : nw + 1;
                continue;
            }
        } else {
            // Method 3: the top bit set means "new width = low byte + 1".
            if (v & (1u << (maxW - 1))) {
                width = int((v + 1) & 0xFF);
                continue;
            }
        }
        // Sign-extend the width-bit value to 8 / 16 bits.
        const int full = is16 ? 16 : 8;
        int32_t s;
        if (width < full) {
            const int sh = 32 - width;
            s = int32_t(v << sh) >> sh;
        } else {
            s = is16 ? int32_t(int16_t(uint16_t(v))) : int32_t(int8_t(uint8_t(v)));
        }
        d1 += uint32_t(s);
        d2 += d1;
        const uint32_t o = it215 ? d2 : d1;
        out[written++] = is16 ? int16_t(uint16_t(o)) : int16_t(uint16_t(uint8_t(o)) << 8);
    }
    rd.seek(blockEnd);
    left -= count;
    return int32_t(count);
}

}  // namespace itfmt

// ---------------------------------------------------------------------------

bool ItImport::fail(const char* msg) {
    std::snprintf(err_, sizeof(err_), "%s", msg);
    abort();
    stage_ = kIdle;
    return false;
}

bool ItImport::begin(IStorage& fs, const char* path, uint8_t* scratch, uint32_t scratchBytes) {
    *this = ItImport{};
    fs_ = &fs;
    if (!scratch || scratchBytes < kScratchBytes) return fail("No memory for the import");
    scratch_ = scratch;
    smpPtr_ = reinterpret_cast<uint32_t*>(scratch);
    patPtr_ = smpPtr_ + 256;
    io_ = scratch + 2048;
    uint8_t* rdBuf = io_ + kIoBytes;   // 2048 + 0x600 + 0x200 = 0x1000
    h_ = fs.open(path, IStorage::kRead);
    if (h_ < 0) return fail("Cannot open file");
    const int32_t sz = fs.size(h_);
    fileSize_ = sz > 0 ? uint32_t(sz) : 0;
    rd_.bind(fs, h_, rdBuf, 0x200);
    uint8_t* hd = io_;
    if (!modfmt::readAt(fs, h_, 0, hd, 0xC0)) return fail("Cannot read file");
    if (std::memcmp(hd, "IMPM", 4) != 0) return fail("Not an IT file");
    ordNum_ = rdU16(hd + 0x20);
    insNum_ = rdU16(hd + 0x22);
    smpNum_ = rdU16(hd + 0x24);
    patNum_ = rdU16(hd + 0x26);
    cmwt_ = rdU16(hd + 0x2A);
    flags_ = rdU16(hd + 0x2C);
    is_ = hd[0x32];
    it_ = hd[0x33];
    // spec 11 §1.3 step 1: Cmwt < 0x200 is refused (old instrument format);
    // sample-mode files do not use instruments, so they are accepted.
    if (instrumentMode() && cmwt_ < 0x200) return fail("Old IT format is not supported");
    if (ordNum_ > 256 || smpNum_ > 255 || patNum_ > 256 || insNum_ > 255) return fail("Damaged IT file");
    // Orders, then the three pointer tables (ITTECH).
    uint8_t ord[256];
    if (!modfmt::readAt(fs, h_, 0xC0, ord, ordNum_)) return fail("Cannot read file");
    const uint32_t insTab = 0xC0u + ordNum_;
    const uint32_t smpTab = insTab + 4u * insNum_;
    const uint32_t patTab = smpTab + 4u * smpNum_;
    std::memset(smpPtr_, 0, 2048);
    nIns_ = instrumentMode() ? (insNum_ < kSampleInstrumentCount ? insNum_ : kSampleInstrumentCount)
                             : (smpNum_ < kSampleInstrumentCount ? smpNum_ : kSampleInstrumentCount);
    nPat_ = patNum_ < kPatternCount ? patNum_ : kPatternCount;
    uint8_t tmp[4];
    for (int i = 0; i < nIns_ && instrumentMode(); ++i) {
        if (!modfmt::readAt(fs, h_, insTab + 4u * uint32_t(i), tmp, 4)) return fail("Cannot read file");
        insPtr_[i] = rdU32(tmp);
    }
    for (int s = 0; s < smpNum_; ++s) {
        if (!modfmt::readAt(fs, h_, smpTab + 4u * uint32_t(s), tmp, 4)) return fail("Cannot read file");
        smpPtr_[s] = rdU32(tmp);
    }
    for (int k = 0; k < nPat_; ++k) {
        if (!modfmt::readAt(fs, h_, patTab + 4u * uint32_t(k), tmp, 4)) return fail("Cannot read file");
        patPtr_[k] = rdU32(tmp);
    }
    // Song: "+++" (0xFE) is skipped, "---" (0xFF) ends it.
    songLen_ = 0;
    for (int i = 0; i < ordNum_ && songLen_ < kSongSlots; ++i) {
        const uint8_t o = ord[i];
        if (o == 0xFF) break;
        if (o == 0xFE || o >= kPatternCount) continue;
        song_[songLen_++] = o;
    }
    readBytes_ = 0xC0u + ordNum_ + 4u * (insNum_ + smpNum_ + patNum_);
    return true;
}

void ItImport::apply(const ModuleTarget& t) {
    t_ = t;
    Project& pr = *t_.project;
    for (int i = 0; i < kSongSlots; ++i) pr.playlist[i] = i < songLen_ ? uint8_t(song_[i] + 1) : 0;
    pr.playlistPos = 0;
    pr.setCurrentPattern(1);
    ts_.speed = is_ ? is_ : modfmt::kDefaultSpeed;
    ts_.tempo = it_ ? it_ : modfmt::kDefaultTempo;
    projBpm_ = modfmt::clampProjectTempo(ts_.bpm());
    pr.setTempo(projBpm_);
    stage_ = kInstr;
    insIdx_ = 0;
}

// spec 11 §1.3 steps 4/5 for Tracker instrument i. Returns true when sample
// data follows (stage kData).
bool ItImport::loadInstrument(int i) {
    Instrument& in = t_.instruments[i];
    in.setDefault();
    t_.present[i] = true;
    int s = 0;
    int vol = 50;
    uint8_t* b = io_;
    if (instrumentMode()) {
        const uint32_t p = insPtr_[i];
        if (!modfmt::readAt(*fs_, h_, p, b, 0x130)) return false;
        readBytes_ += 0x130;
        // Sample choice: the first non-zero entry of the note->sample table.
        for (int n = 0; n < 120 && !s; ++n) s = b[0x41 + 2 * n];
        const int gbv = b[0x18] > 128 ? 128 : b[0x18];
        vol = gbv * 50 >> 7;
        modfmt::moduleName(b + 0x20, 26, in.name, sizeof(in.name));
        if (!s && i < smpNum_ && smpPtr_[i]) s = i + 1;
    } else {
        s = i + 1;
    }
    in.volume = uint8_t(vol);
    if (s < 1 || s > smpNum_ || !smpPtr_[s - 1]) {
        if (!instrumentMode()) modfmt::moduleName(reinterpret_cast<const uint8_t*>(""), 0, in.name, sizeof(in.name));
        return false;
    }
    if (!modfmt::readAt(*fs_, h_, smpPtr_[s - 1], b, 0x50)) return false;
    readBytes_ += 0x50;
    const uint8_t gvl = b[0x11] > 64 ? 64 : b[0x11];
    const uint8_t flg = b[0x12];
    const uint8_t v = b[0x13] > 64 ? 64 : b[0x13];
    if (!instrumentMode()) {
        modfmt::moduleName(b + 0x14, 26, in.name, sizeof(in.name));
        vol = 50 * gvl >> 6;
    }
    in.volume = uint8_t(vol * v >> 6);
    const uint8_t cvt = b[0x2E];
    const uint32_t len = rdU32(b + 0x30);
    if (!(flg & 1) || len == 0) {
        in.active = 0;
        in.lengthFrames = 0;
        return false;
    }
    in.active = 1;
    in.startPoint = 0;
    in.endPoint = 0xFFFF;
    // Loop: the normal loop, else the sustain loop.
    uint32_t lb = 0, le = 0;
    bool loop = false, pp = false;
    if (flg & 0x10) {
        lb = rdU32(b + 0x34);
        le = rdU32(b + 0x38);
        pp = flg & 0x40;
        loop = true;
    } else if (flg & 0x20) {
        lb = rdU32(b + 0x40);
        le = rdU32(b + 0x44);
        pp = flg & 0x80;
        loop = true;
    }
    if (loop) {
        auto clampPt = [](uint64_t x) { return uint16_t(x < 1 ? 1 : (x > 65535 ? 65535 : x)); };
        const uint16_t l1 = clampPt(uint64_t(lb) * 65535 / len);
        const uint16_t l2 = clampPt(uint64_t(le < len ? le : len) * 65535 / len);
        if (l2 > l1) {
            in.playMode = uint8_t(pp ? PlayMode::PingPongLoop : PlayMode::ForwardLoop);
            in.loopPoint1 = l1;
            in.loopPoint2 = l2;
        }
    }
    // Tune against 44100 Hz for both bit depths (spec 11 §1.3 step 5).
    const uint32_t c5 = rdU32(b + 0x3C);
    if (c5 && c5 != itfmt::kRefRate) {
        const float st = 12.0f * std::log2(float(c5) / float(itfmt::kRefRate));
        const float ti = std::trunc(st);
        if (std::fabs(st) < 25.0f) {
            in.tune = int8_t(ti < -24 ? -24 : (ti > 24 ? 24 : ti));
        } else {
            transpose_[i] = int8_t(ti < -127 ? -127 : (ti > 127 ? 127 : ti));
        }
        float ft = std::trunc((st - ti) * 100.0f);
        in.finetune = int8_t(ft < -100 ? -100 : (ft > 100 ? 100 : ft));
    }
    const bool stereo = flg & 4;
    if (memFull_) return false;
    int16_t* dst = t_.reserve(t_.ctx, i, len, stereo);
    if (!dst) {
        memFull_ = true;
        std::snprintf(warn_, sizeof(warn_), "Not enough sample memory");
        return false;
    }
    in.lengthFrames = len;
    in.stereo = stereo;
    ld_ = Load{};
    ld_.dst = dst;
    ld_.frames = len;
    ld_.off = rdU32(b + 0x48);
    ld_.is16 = flg & 2;
    ld_.stereo = stereo;
    ld_.compressed = flg & 8;
    ld_.isSigned = cvt & 1;
    ld_.delta = cvt & 4;
    if (ld_.compressed) {
        rd_.seek(ld_.off);
        rd_.consumed = 0;
        ld_.dec.start(ld_.is16, ld_.delta, len);
    }
    return true;
}

// One chunk / block of the sample being loaded; true when it is complete.
bool ItImport::sampleChunk(uint32_t& cost) {
    Load& l = ld_;
    const uint32_t total = l.frames * (l.stereo ? 2u : 1u);
    if (l.compressed) {
        rd_.consumed = 0;
        const int32_t n = l.dec.decodeBlock(rd_, l.dst + l.pos);
        cost += rd_.consumed;
        readBytes_ += rd_.consumed;
        if (n < 0) {
            std::snprintf(warn_, sizeof(warn_), "Damaged compressed sample");
            return true;   // keep what was decoded; the rest stays silent
        }
        l.pos += uint32_t(n);
        if (l.dec.left == 0 && l.stereo && l.chan == 0) {
            l.chan = 1;
            l.dec.start(l.is16, l.delta, l.frames);   // right channel follows
        }
        return l.pos >= total || n == 0;
    }
    const uint32_t bps = l.is16 ? 2u : 1u;
    uint32_t vals = (total - l.pos);
    if (vals * bps > kIoBytes) vals = kIoBytes / bps;
    uint32_t got = 0;
    if (!modfmt::readAt(*fs_, h_, l.off + l.pos * bps, io_, vals * bps, &got)) {
        std::snprintf(warn_, sizeof(warn_), "Cannot read sample");
        return true;
    }
    if (got < vals * bps) std::snprintf(warn_, sizeof(warn_), "File is truncated");
    for (uint32_t k = 0; k < vals; ++k) {
        const uint32_t idx = l.pos + k;
        if (l.stereo && idx == l.frames) l.acc = 0;   // delta restarts per channel
        uint32_t v = l.is16 ? rdU16(io_ + 2 * k) : io_[k];
        if (l.delta) {
            l.acc += v;
            v = l.acc;
        }
        int16_t s;
        if (l.is16) s = int16_t(uint16_t(v) ^ (l.isSigned ? 0 : 0x8000));
        else s = int16_t(uint16_t(uint8_t(v) ^ (l.isSigned ? 0 : 0x80)) << 8);
        l.dst[idx] = s;
    }
    l.pos += vals;
    cost += vals * bps;
    readBytes_ += vals * bps;
    return l.pos >= total;
}

bool ItImport::nextPattern(int& k, bool& inSong) {
    while (seqPos_ < songLen_) {
        const int q = song_[seqPos_++];
        if (q < nPat_ && !(done_[q >> 3] & (1 << (q & 7)))) {
            k = q;
            inSong = true;
            return true;
        }
        if (q >= nPat_ && q < kPatternCount && !(done_[q >> 3] & (1 << (q & 7)))) {
            k = q;   // referenced but not stored: empty pattern
            inSong = true;
            return true;
        }
    }
    while (rest_ < nPat_) {
        const int q = rest_++;
        if (!(done_[q >> 3] & (1 << (q & 7)))) {
            ts_.speed = is_ ? is_ : modfmt::kDefaultSpeed;
            ts_.tempo = it_ ? it_ : modfmt::kDefaultTempo;
            k = q;
            inSong = false;
            return true;
        }
    }
    return false;
}

void ItImport::storeCell(PatternStock& pat, int row, int ch, int note, int ins, int vol, int cmd, int par,
                         bool& row0Tempo) {
    if (ins > 0) chanInstr_[ch] = uint8_t(ins);
    // Speed / tempo state follows every channel, stored or not.
    bool tempoFx = false;
    if (cmd == 1 && par > 0) {
        ts_.speed = par;
        tempoFx = true;
    } else if (cmd == 20 && par >= 0x20) {
        ts_.tempo = par;
        tempoFx = true;
    }
    if (tempoFx && row == 0) row0Tempo = true;
    if (row >= itfmt::kMaxRows || ch >= itfmt::kMaxChannels) return;
    Step& st = pat.tracks[ch].steps[row];
    const int curIns = chanInstr_[ch];
    if (note >= 0 && note <= 119) {
        if (curIns >= 1 && curIns <= nIns_) {
            int n = note + transpose_[curIns - 1];
            st.note = int8_t(n < 0 ? 0 : (n > kNoteMax ? kNoteMax : n));
            st.instrument = uint8_t(curIns - 1);
        }
    } else if (note == 255 || note == 254 || (note >= 120 && note <= 253)) {
        st.note = note == 255 ? kNoteOff : (note == 254 ? kNoteCut : kNoteFade);
        st.instrument = uint8_t(curIns >= 1 && curIns <= nIns_ ? curIns - 1 : 0);
    }
    // Volume column -> FX slot 1 (spec 11 §1.3 step 6).
    if (vol >= 0 && vol <= 64) {
        st.fx[1].type = uint8_t(FxId::Volume);
        st.fx[1].value = uint8_t(vol * 25 >> 5);
    } else if (vol >= 128 && vol <= 192) {
        st.fx[1].type = uint8_t(FxId::Panning);
        st.fx[1].value = uint8_t((vol - 128) * 25 >> 4);
    }
    // Effects -> FX slot 0.
    StepFx& fx = st.fx[0];
    switch (cmd) {
        case 1:    // Axx speed
        case 20:   // Txx tempo
            if (tempoFx) {
                fx.type = uint8_t(FxId::Tempo);
                fx.value = modfmt::tempoFxValue(ts_.bpm());
            }
            break;
        case 3:    // Cxx pattern break
            fx.type = uint8_t(FxId::BreakPattern);
            fx.value = 1;
            break;
        case 7:    // Gxx tone portamento
            if (par) lastG_[ch] = uint8_t(par);
            fx.type = uint8_t(FxId::Glide);
            fx.value = lastG_[ch] > 100 ? 100 : lastG_[ch];
            break;
        case 15: {   // Oxx sample offset
            if (par) lastO_[ch] = uint8_t(par);
            const uint32_t frames = curIns >= 1 && curIns <= nIns_ ? t_.instruments[curIns - 1].lengthFrames : 0;
            fx.type = uint8_t(FxId::Position);
            fx.value = modfmt::positionFxValue(uint32_t(lastO_[ch]) * 256u, frames);
            break;
        }
        case 24:   // Xxx set panning
            fx.type = uint8_t(FxId::Panning);
            fx.value = uint8_t(par * 100 / 255);
            break;
        default:
            break;
    }
}

bool ItImport::convertPattern(int k, bool inSong, PatternStock& pat, uint32_t& cost) {
    pat.setDefault();
    const uint32_t ptr = k < nPat_ ? patPtr_[k] : 0;
    const float entryBpm = ts_.bpm();
    bool row0Tempo = false;
    if (!ptr) {
        // ITTECH: pointer 0 = an empty 64-row pattern.
        for (auto& t : pat.tracks) t.lastStep = 63;
    } else {
        uint8_t hd[8];
        if (!modfmt::readAt(*fs_, h_, ptr, hd, 8)) return false;
        const uint32_t len = rdU16(hd);
        int rows = rdU16(hd + 2);
        if (rows < 1) rows = 64;
        const int last = (rows > itfmt::kMaxRows ? itfmt::kMaxRows : rows) - 1;
        for (auto& t : pat.tracks) t.lastStep = uint8_t(last);
        // Packing memory is per channel and restarts with each pattern.
        uint8_t lastMask[64] = {}, lastNote[64] = {}, lastIns[64] = {}, lastVol[64] = {}, lastCmd[64] = {},
                lastPar[64] = {};
        rd_.seek(ptr + 8);
        rd_.consumed = 0;
        int row = 0;
        while (row < rows && row < itfmt::kMaxRows && rd_.consumed < len && !rd_.err) {
            const uint8_t cv = rd_.next();
            if (!cv) {
                ++row;
                continue;
            }
            const int ch = (cv - 1) & 63;
            uint8_t mask = lastMask[ch];
            if (cv & 0x80) mask = lastMask[ch] = rd_.next();
            int note = -1, ins = 0, vol = -1, cmd = 0, par = 0;
            if (mask & 1) note = lastNote[ch] = rd_.next();
            if (mask & 2) ins = lastIns[ch] = rd_.next();
            if (mask & 4) vol = lastVol[ch] = rd_.next();
            if (mask & 8) {
                cmd = lastCmd[ch] = rd_.next();
                par = lastPar[ch] = rd_.next();
            }
            if (mask & 16) note = lastNote[ch];
            if (mask & 32) ins = lastIns[ch];
            if (mask & 64) vol = lastVol[ch];
            if (mask & 128) {
                cmd = lastCmd[ch];
                par = lastPar[ch];
            }
            storeCell(pat, row, ch, note, ins, vol, cmd, par, row0Tempo);
        }
        cost += rd_.consumed + 8;
        readBytes_ += rd_.consumed + 8;
        if (rd_.err) return false;
    }
    // The Tempo FX override ends at every song-pattern switch (spec 03
    // §3.6): restate the tempo on row 0 when it differs from the project's.
    if (inSong && !row0Tempo && std::fabs(entryBpm - projBpm_) > 0.01f) {
        for (int t = 0; t < kStockTrackCount; ++t) {
            Step& s0 = pat.tracks[t].steps[0];
            StepFx* fx = s0.fx[0].type == 0 ? &s0.fx[0] : (s0.fx[1].type == 0 ? &s0.fx[1] : nullptr);
            if (!fx) continue;
            fx->type = uint8_t(FxId::Tempo);
            fx->value = modfmt::tempoFxValue(entryBpm);
            break;
        }
    }
    return true;
}

ImportStep ItImport::step(uint32_t& cost) {
    switch (stage_) {
        case kInstr:
            if (insIdx_ >= nIns_) {
                stage_ = kPatterns;
                return ImportStep::More;
            }
            cost += 0x180;
            if (loadInstrument(insIdx_)) stage_ = kData;
            else ++insIdx_;
            return ImportStep::More;
        case kData:
            if (sampleChunk(cost)) {
                stage_ = kInstr;
                ++insIdx_;
            }
            return ImportStep::More;
        case kPatterns: {
            int k = 0;
            bool inSong = false;
            if (!nextPattern(k, inSong)) {
                finish();
                return ImportStep::Done;
            }
            done_[k >> 3] |= uint8_t(1 << (k & 7));
            PatternStock* pat = t_.pattern(t_.ctx, k + 1);
            if (pat && !convertPattern(k, inSong, *pat, cost)) {
                fail("Cannot read file");
                return ImportStep::Failed;
            }
            return ImportStep::More;
        }
        case kDone:
            return ImportStep::Done;
        default:
            return ImportStep::Failed;
    }
}

void ItImport::finish() {
    abort();
    stage_ = kDone;
}

int ItImport::percent() const {
    if (stage_ == kDone) return 100;
    const uint64_t p = uint64_t(readBytes_) * 100 / (fileSize_ ? fileSize_ : 1);
    return p > 99 ? 99 : int(p);
}

void ItImport::abort() {
    if (fs_ && h_ >= 0) fs_->close(h_);
    h_ = -1;
}

}  // namespace ot
