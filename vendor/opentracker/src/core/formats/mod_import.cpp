// ProTracker MOD import (spec 11 §1.1, §1.2) and shared module helpers.
//
// Deviations from stock 1.9.2 (all spec 11 defects; reimpl/README.md lists them):
// - every 4-channel signature (M.K., M!K!, FLT4, 4CHN), the 15-sample
//   Soundtracker layout (130-byte order block) and 2..8-channel files are
//   read; more channels are refused with a message;
// - the samples are converted to 16 bit at full length and notes use
//   index + 7 (stock: raw bytes as half-length int16 frames, index - 5);
// - finetuned / extended periods map to the nearest note, the sample
//   finetune becomes the instrument finetune;
// - the song is the MOD song length (stock: all 128 orders);
// - a one-pattern module reads one pattern (stock: max(1, max order) + 1);
// - Fxx: speed and tempo both convert to BPM = 6 * tempo / speed, written
//   as Tempo FX v = BPM / 2 (stock: v = xx, i.e. double tempo; speed lost;
//   F20 was taken as a speed); the project tempo is the song's start BPM;
// - 9xx sample offset is relative to the sample length (stock: xx / 256).
#include "formats/mod_import.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "model/fx.h"

namespace ot {

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool containsNoCase(const char* s, const char* sub) {
    const size_t n = std::strlen(sub);
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && lower(s[i]) == sub[i]) ++i;
        if (i == n) return true;
    }
    return false;
}

uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

constexpr uint32_t kHdr31 = 1084;   // 20 + 31*30 + 2 + 128 + 4
constexpr uint32_t kHdr15 = 600;    // 20 + 15*30 + 2 + 128
constexpr uint32_t kChunk = 2048;   // sample bytes per step
}  // namespace

bool moduleListFilter(const char* name) {
    return name && (containsNoCase(name, ".mod") || containsNoCase(name, ".it"));
}

ModuleKind moduleKind(const char* name) {
    if (!name) return ModuleKind::None;
    if (containsNoCase(name, ".mod")) return ModuleKind::Mod;
    if (containsNoCase(name, ".it")) return ModuleKind::It;
    return ModuleKind::None;
}

void moduleProjectName(const char* fileName, char* out, uint32_t cap) {
    if (!cap) return;
    uint32_t n = 0;
    while (fileName && fileName[n] && fileName[n] != '.' && n < mt::kNameMax && n + 1 < cap) {
        out[n] = fileName[n];
        ++n;
    }
    out[n] = 0;
}

namespace modfmt {

bool readAt(IStorage& fs, IStorage::Handle h, uint32_t off, void* dst, uint32_t len, uint32_t* got) {
    auto* d = static_cast<uint8_t*>(dst);
    std::memset(d, 0, len);
    if (got) *got = 0;
    if (!fs.seek(h, off)) return true;   // past the end: zeros
    uint32_t n = 0;
    while (n < len) {
        const int32_t r = fs.read(h, d + n, len - n);
        if (r < 0) return false;
        if (r == 0) break;
        n += uint32_t(r);
    }
    if (got) *got = n;
    return true;
}

int periodToNote(uint16_t period) {
    if (period == 0) return -1;
    const float st = 12.0f * std::log2(428.0f / float(period));
    const int note = 31 + int(std::lround(st));
    return note >= 0 && note <= kNoteMax ? note : -1;
}

int8_t finetuneCents(uint8_t nibble) {
    const int v = (nibble & 7) - (nibble & 8);   // -8..7 eighths of a semitone
    const int c = v * 25;                        // x 12.5, rounded half away from 0
    return int8_t(c >= 0 ? (c + 1) / 2 : (c - 1) / 2);
}

float trackerBpm(int speed, int tempo) {
    if (speed < 1) speed = kDefaultSpeed;
    return 6.0f * float(tempo) / float(speed);
}

uint8_t tempoFxValue(float bpm) {
    int v = int(std::lround(bpm / 2.0f));
    if (v < 5) v = 5;
    if (v > 200) v = 200;
    return uint8_t(v);
}

float clampProjectTempo(float bpm) {
    // spec 03 §1.2 ".it import clamp" 10..400 (also used for MOD).
    return bpm < 10.0f ? 10.0f : (bpm > 400.0f ? 400.0f : bpm);
}

uint8_t positionFxValue(uint32_t offsetFrames, uint32_t lengthFrames) {
    if (!lengthFrames) return 0;
    const uint64_t v = uint64_t(offsetFrames) * 100 / lengthFrames;
    return uint8_t(v > 100 ? 100 : v);
}

void moduleName(const uint8_t* src, uint32_t len, char* dst, uint32_t cap) {
    if (!cap) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && src[i] && n + 1 < cap; ++i) dst[n++] = (src[i] >= 32 && src[i] < 127) ? char(src[i]) : ' ';
    while (n && dst[n - 1] == ' ') --n;
    uint32_t lead = 0;
    while (lead < n && dst[lead] == ' ') ++lead;
    if (lead) {
        std::memmove(dst, dst + lead, n - lead);
        n -= lead;
    }
    if (!n && cap > 1) dst[n++] = '-';   // spec 11 §1.2 step 2: "-" if empty
    dst[n] = 0;
}

}  // namespace modfmt

// ---------------------------------------------------------------------------

bool ModImport::begin(IStorage& fs, const char* path, uint8_t* scratch, uint32_t scratchBytes) {
    *this = ModImport{};
    fs_ = &fs;
    if (!scratch || scratchBytes < kScratchBytes) {
        std::snprintf(err_, sizeof(err_), "No memory for the import");
        return false;
    }
    buf_ = scratch;
    h_ = fs.open(path, IStorage::kRead);
    if (h_ < 0) {
        std::snprintf(err_, sizeof(err_), "Cannot open file");
        return false;
    }
    const int32_t sz = fs.size(h_);
    fileSize_ = sz > 0 ? uint32_t(sz) : 0;
    uint8_t* hd = buf_;
    if (!modfmt::readAt(fs, h_, 0, hd, kHdr31)) {
        std::snprintf(err_, sizeof(err_), "Cannot read file");
        abort();
        return false;
    }
    // Signature at 1080 (public mod-form; stock knows only "M.K.", spec 11 §1.2 step 0).
    const uint8_t* s = hd + 1080;
    int ch = 0;
    if (!std::memcmp(s, "M.K.", 4) || !std::memcmp(s, "M!K!", 4) || !std::memcmp(s, "FLT4", 4) ||
        !std::memcmp(s, "4CHN", 4))
        ch = 4;
    else if (!std::memcmp(s, "FLT8", 4) || !std::memcmp(s, "OKTA", 4) || !std::memcmp(s, "OCTA", 4) ||
             !std::memcmp(s, "CD81", 4))
        ch = 8;
    else if (isDigit(s[0]) && !std::memcmp(s + 1, "CHN", 3))
        ch = s[0] - '0';
    else if (isDigit(s[0]) && isDigit(s[1]) && (!std::memcmp(s + 2, "CH", 2) || !std::memcmp(s + 2, "CN", 2)))
        ch = (s[0] - '0') * 10 + (s[1] - '0');
    else if (!std::memcmp(s, "TDZ", 3) && isDigit(s[3]))
        ch = s[3] - '0';
    if (ch) {
        nSmp_ = 31;
        patOff_ = kHdr31;
    } else {
        // 15-sample Soundtracker module: no signature, 130-byte order block.
        nSmp_ = 15;
        patOff_ = kHdr15;
        ch = 4;
        bool ok = fileSize_ >= kHdr15;
        for (int k = 0; k < 15 && ok; ++k) ok = hd[20 + 30 * k + 25] <= 64;
        const uint8_t len = hd[20 + 30 * 15];
        ok = ok && len >= 1 && len <= 128;
        for (int i = 0; i < 128 && ok; ++i) ok = hd[20 + 30 * 15 + 2 + i] < 128;
        if (!ok) {
            std::snprintf(err_, sizeof(err_), "Unknown module format");
            abort();
            return false;
        }
    }
    if (ch < 1 || ch > modfmt::kMaxModChannels) {
        std::snprintf(err_, sizeof(err_), "%d-channel module; up to %d are supported", ch, modfmt::kMaxModChannels);
        abort();
        return false;
    }
    ch_ = ch;
    for (int k = 0; k < nSmp_; ++k) {
        const uint8_t* h = hd + 20 + 30 * k;
        std::memcpy(smpHdr_[k], h, 30);
        Smp& m = smp_[k];
        m.words = be16(h + 22);
        m.fine = h[24] & 0x0F;
        m.vol = h[25];
        m.repStart = be16(h + 26);
        m.repLen = be16(h + 28);
    }
    const uint8_t* ord = hd + 20 + 30 * nSmp_;
    songLen_ = ord[0] < 1 ? 1 : (ord[0] > 128 ? 128 : ord[0]);
    int maxPat = 0;
    for (int i = 0; i < 128; ++i) {
        orders_[i] = ord[2 + i] & 0x7F;
        if (orders_[i] > maxPat) maxPat = orders_[i];   // all 128 entries (public format)
    }
    nPat_ = maxPat + 1;
    dataOff_ = patOff_ + uint32_t(nPat_) * 64u * uint32_t(ch_) * 4u;
    if (dataOff_ > fileSize_) {
        std::snprintf(err_, sizeof(err_), "File is truncated");
        abort();
        return false;
    }
    std::memcpy(name_, hd, 20);
    // Units: one per pattern plus one per sample chunk.
    total_ = uint32_t(nPat_);
    for (int k = 0; k < nSmp_; ++k) total_ += (smp_[k].words * 2 + kChunk - 1) / kChunk;
    if (!total_) total_ = 1;
    return true;
}

void ModImport::apply(const ModuleTarget& t) {
    t_ = t;
    // spec 11 §1.2 step 2: instrument k := defaults + MOD sample header.
    for (int k = 0; k < kSampleInstrumentCount; ++k) {
        if (k >= nSmp_) continue;   // spec: instruments nSmp+1..48 keep the new-project defaults
        Instrument& in = t_.instruments[k];
        const Smp& m = smp_[k];
        in.setDefault();
        in.active = 1;
        in.sampleType = uint8_t(SampleType::WaveFile);
        modfmt::moduleName(smpHdr_[k], 22, in.name, sizeof(in.name));
        in.volume = uint8_t((m.vol > 64 ? 64 : m.vol) * 50 >> 6);
        in.finetune = modfmt::finetuneCents(m.fine);
        const uint32_t len = m.words;   // ratios: words are fine for the loop points
        in.lengthFrames = 0;            // set when the data is loaded
        in.startPoint = 0;
        in.endPoint = 0xFFFF;
        if (m.repLen > 1 && len) {
            const uint32_t a = m.repStart < len ? m.repStart : len;
            const uint32_t b = uint32_t(m.repStart) + m.repLen < len ? uint32_t(m.repStart) + m.repLen : len;
            const uint32_t l1 = a * 65535u / len, l2 = b * 65535u / len;
            if (l2 > l1) {
                in.playMode = uint8_t(PlayMode::ForwardLoop);
                in.loopPoint1 = uint16_t(l1);
                in.loopPoint2 = uint16_t(l2);
            }
        }
        t_.present[k] = true;
    }
    // Song: the MOD song length, pattern p -> Tracker pattern p+1 (spec 11 §1.2 step 4).
    Project& pr = *t_.project;
    for (int i = 0; i < kSongSlots; ++i) pr.playlist[i] = i < songLen_ ? uint8_t(orders_[i] + 1) : 0;
    pr.playlistPos = 0;
    pr.setCurrentPattern(1);
    for (int c = 0; c < modfmt::kMaxModChannels; ++c) lastInstr_[c] = 47;   // spec: starts at instrument 48
    ts_ = modfmt::TempoState{};
    projBpm_ = modfmt::clampProjectTempo(ts_.bpm());
    pr.setTempo(projBpm_);
    stage_ = kPatterns;
}

// Patterns go in song order first, so speed/tempo carry over like playback;
// patterns outside the song follow at the default speed/tempo.
bool ModImport::nextPattern(int& p) {
    while (seqPos_ < songLen_) {
        const int q = orders_[seqPos_++];
        if (!(done_[q >> 3] & (1 << (q & 7)))) {
            p = q;
            return true;
        }
    }
    while (rest_ < nPat_) {
        const int q = rest_++;
        if (!(done_[q >> 3] & (1 << (q & 7)))) {
            ts_ = modfmt::TempoState{};
            p = q;
            return true;
        }
    }
    return false;
}

void ModImport::convertPattern(PatternStock& pat, bool first) {
    pat.setDefault();
    for (int t = 0; t < 8; ++t) pat.tracks[t].lastStep = 63;   // spec 11 §1.2 step 4
    const float entryBpm = ts_.bpm();
    bool row0Tempo = false;
    for (int r = 0; r < 64; ++r) {
        for (int c = 0; c < ch_; ++c) {
            const uint8_t* b = buf_ + (r * ch_ + c) * 4;
            const uint16_t period = uint16_t(((b[0] & 0x0F) << 8) | b[1]);
            const uint8_t smp = uint8_t((b[0] & 0xF0) | (b[2] >> 4));
            const uint8_t eff = b[2] & 0x0F, par = b[3];
            Step& st = pat.tracks[c].steps[r];
            if (smp) lastInstr_[c] = uint8_t((smp > 47 ? 47 : smp) - 1);
            const int note = modfmt::periodToNote(period);
            if (note >= 0) {
                st.note = int8_t(note);
                st.instrument = lastInstr_[c];
            }
            StepFx& fx = st.fx[1];   // spec 11: FX slot 1 (step bytes +2/+3)
            switch (eff) {
                case 0x3:
                    fx.type = uint8_t(FxId::Glide);
                    fx.value = par > 100 ? 100 : par;
                    break;
                case 0x9: {
                    const int in = lastInstr_[c];
                    const uint32_t frames = in < nSmp_ ? smp_[in].words * 2u : 0;
                    fx.type = uint8_t(FxId::Position);
                    fx.value = modfmt::positionFxValue(uint32_t(par) * 256u, frames);
                    break;
                }
                case 0xC:
                    fx.type = uint8_t(FxId::Volume);
                    fx.value = uint8_t((par > 64 ? 64 : par) * 50 >> 6);
                    break;
                case 0xD:
                    fx.type = uint8_t(FxId::BreakPattern);
                    fx.value = 1;
                    break;
                case 0xF:
                    if (!par) break;
                    if (par < 0x20) ts_.speed = par;
                    else ts_.tempo = par;
                    fx.type = uint8_t(FxId::Tempo);
                    fx.value = modfmt::tempoFxValue(ts_.bpm());
                    if (r == 0) row0Tempo = true;
                    break;
                default:
                    break;   // spec 11 table: other effects are dropped
            }
        }
        if (r == 0 && first && !projTempoSet_) {
            // The song's start tempo becomes the project tempo.
            projBpm_ = modfmt::clampProjectTempo(ts_.bpm());
            t_.project->setTempo(projBpm_);
            projTempoSet_ = true;
        }
    }
    // The Tempo FX override ends at every song-pattern switch (spec 03
    // §3.6), so a pattern entered at another tempo restates it on row 0.
    if (!first && !row0Tempo && std::fabs(entryBpm - projBpm_) > 0.01f) {
        StepFx& fx = pat.tracks[0].steps[0].fx[0];
        fx.type = uint8_t(FxId::Tempo);
        fx.value = modfmt::tempoFxValue(entryBpm);
    }
}

ImportStep ModImport::step(uint32_t& cost) {
    switch (stage_) {
        case kPatterns: {
            int p = 0;
            if (!nextPattern(p)) {
                stage_ = kSamples;
                smpIdx_ = 0;
                smpPos_ = 0;
                smpOff_ = dataOff_;
                return ImportStep::More;
            }
            const uint32_t bytes = 64u * uint32_t(ch_) * 4u;
            if (!modfmt::readAt(*fs_, h_, patOff_ + uint32_t(p) * bytes, buf_, bytes)) {
                std::snprintf(err_, sizeof(err_), "Cannot read file");
                abort();
                return ImportStep::Failed;
            }
            const bool first = !projTempoSet_;
            PatternStock* pat = t_.pattern(t_.ctx, p + 1);
            if (pat) convertPattern(*pat, first);
            done_[p >> 3] |= uint8_t(1 << (p & 7));
            cost += bytes;
            ++units_;
            return ImportStep::More;
        }
        case kSamples: {
            if (smpIdx_ >= nSmp_) {
                finish();
                return ImportStep::Done;
            }
            const uint32_t bytes = smp_[smpIdx_].words * 2u;
            Instrument& in = t_.instruments[smpIdx_];
            if (smpPos_ == 0) {
                dst_ = nullptr;
                if (bytes && !memFull_) {
                    dst_ = t_.reserve(t_.ctx, smpIdx_, bytes, false);
                    if (!dst_) {
                        memFull_ = true;
                        std::snprintf(warn_, sizeof(warn_), "Not enough sample memory");
                    }
                }
                if (!dst_) {
                    // Empty or no room: skip this sample's data.
                    units_ += (bytes + kChunk - 1) / kChunk;
                    smpOff_ += bytes;
                    ++smpIdx_;
                    return ImportStep::More;
                }
                in.lengthFrames = bytes;
            }
            const uint32_t n = bytes - smpPos_ < kChunk ? bytes - smpPos_ : kChunk;
            uint32_t got = 0;
            if (!modfmt::readAt(*fs_, h_, smpOff_ + smpPos_, buf_, n, &got)) {
                std::snprintf(err_, sizeof(err_), "Cannot read file");
                abort();
                return ImportStep::Failed;
            }
            if (got < n) std::snprintf(warn_, sizeof(warn_), "File is truncated");
            // 8-bit signed PCM -> int16 (spec 11 §1.2 step 5 "converts properly").
            for (uint32_t i = 0; i < n; ++i) dst_[smpPos_ + i] = int16_t(uint16_t(buf_[i]) << 8);
            smpPos_ += n;
            cost += n;
            ++units_;
            if (smpPos_ >= bytes) {
                smpOff_ += bytes;
                smpPos_ = 0;
                ++smpIdx_;
            }
            return ImportStep::More;
        }
        case kDone:
            return ImportStep::Done;
        default:
            return ImportStep::Failed;
    }
}

void ModImport::finish() {
    abort();   // close the file
    stage_ = kDone;
    units_ = total_;
}

int ModImport::percent() const {
    const uint32_t p = units_ * 100u / (total_ ? total_ : 1);
    return p > 100 ? 100 : int(p);
}

void ModImport::abort() {
    if (fs_ && h_ >= 0) fs_->close(h_);
    h_ = -1;
}

}  // namespace ot
