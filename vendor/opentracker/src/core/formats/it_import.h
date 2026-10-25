// File > Import Mod: Impulse Tracker IT import (spec 11 §1.3).
//
// Structures follow the public ITTECH.TXT (IMPM header, IMPI instrument,
// IMPS sample, packed patterns); IT 2.14/2.15 sample compression follows
// the public description of that scheme. Same cooperative interface as
// ModImport (mod_import.h).
#pragma once

#include <cstdint>

#include "formats/mod_import.h"

namespace ot {

namespace itfmt {
constexpr int kMaxChannels = kStockTrackCount;   // spec 11 §1.4: 12 channels, more are dropped
constexpr int kMaxRows = kStepCount;        // spec 11 §1.4: <= 128 rows
constexpr uint32_t kRefRate = 44100;        // C5Speed of unity pitch (8- and 16-bit)

// Buffered sequential reader at absolute file offsets (zeros past the end).
struct ByteReader {
    IStorage* fs = nullptr;
    IStorage::Handle h = -1;
    uint8_t* buf = nullptr;
    uint32_t cap = 0;
    uint32_t base = 0, n = 0, i = 0;
    bool err = false;
    uint32_t consumed = 0;   // bytes delivered since bind()
    void bind(IStorage& f, IStorage::Handle hd, uint8_t* b, uint32_t c) {
        fs = &f;
        h = hd;
        buf = b;
        cap = c;
        n = i = 0;
    }
    void seek(uint32_t off) {
        base = off;
        n = i = 0;
    }
    uint32_t tell() const { return base + i; }
    uint8_t next();
};

// One channel of an IT 2.14 (it215 = false) or 2.15 compressed sample.
// decodeBlock() reads one compressed block (u16 length + bit stream) and
// writes up to 0x8000 (8-bit) / 0x4000 (16-bit) frames as int16.
struct ItDecompressor {
    bool is16 = false, it215 = false;
    uint32_t left = 0;   // frames still to decode in this channel
    void start(bool sixteen, bool v215, uint32_t frames) {
        is16 = sixteen;
        it215 = v215;
        left = frames;
    }
    // Returns frames written, 0 when done; < 0 on a corrupt stream.
    int32_t decodeBlock(ByteReader& rd, int16_t* out);
};
}  // namespace itfmt

class ItImport {
public:
    static constexpr uint32_t kScratchBytes = 0x1000;
    bool begin(IStorage& fs, const char* path, uint8_t* scratch, uint32_t scratchBytes);
    void apply(const ModuleTarget& t);
    ImportStep step(uint32_t& cost);
    int percent() const;
    void abort();
    const char* error() const { return err_; }
    const char* warning() const { return warn_; }

    bool instrumentMode() const { return (flags_ & 4) != 0; }
    int instrumentCount() const { return nIns_; }
    int patternCount() const { return nPat_; }
    int8_t transpose(int i) const { return i >= 0 && i < kSampleInstrumentCount ? transpose_[i] : 0; }

private:
    enum Stage : uint8_t { kIdle, kInstr, kData, kPatterns, kDone };
    bool loadInstrument(int i);
    bool sampleChunk(uint32_t& cost);
    bool nextPattern(int& k, bool& inSong);
    bool convertPattern(int k, bool inSong, PatternStock& pat, uint32_t& cost);
    void storeCell(PatternStock& pat, int row, int ch, int note, int ins, int vol, int cmd, int par, bool& row0Tempo);
    void finish();
    bool fail(const char* msg);

    IStorage* fs_ = nullptr;
    IStorage::Handle h_ = -1;
    ModuleTarget t_;
    uint8_t* scratch_ = nullptr;
    uint32_t* smpPtr_ = nullptr;   // scratch, 256 entries
    uint32_t* patPtr_ = nullptr;   // scratch, 256 entries
    uint8_t* io_ = nullptr;        // scratch, kIoBytes
    static constexpr uint32_t kIoBytes = 0x600;
    itfmt::ByteReader rd_;
    uint32_t fileSize_ = 0, readBytes_ = 0;
    uint16_t ordNum_ = 0, insNum_ = 0, smpNum_ = 0, patNum_ = 0, cmwt_ = 0, flags_ = 0;
    uint8_t is_ = 6, it_ = 125;
    uint32_t insPtr_[kSampleInstrumentCount] = {};
    uint8_t song_[kSongSlots] = {};
    int songLen_ = 0;
    int nIns_ = 0, nPat_ = 0;
    int8_t transpose_[kSampleInstrumentCount] = {};
    Stage stage_ = kIdle;
    int insIdx_ = 0;
    bool memFull_ = false;
    // Sample data being loaded.
    struct Load {
        int16_t* dst = nullptr;
        uint32_t frames = 0, pos = 0, off = 0;   // pos counts int16 values (both channels)
        bool is16 = false, stereo = false, compressed = false, isSigned = true, delta = false;
        int chan = 0;
        uint32_t acc = 0;
        itfmt::ItDecompressor dec;
    } ld_;
    // Pattern conversion state.
    uint8_t done_[32] = {};
    int seqPos_ = 0, rest_ = 0;
    uint8_t chanInstr_[64] = {};   // playback instrument per channel (1-based, 0 = none)
    uint8_t lastG_[64] = {}, lastO_[64] = {};
    modfmt::TempoState ts_;
    float projBpm_ = 0;
    char err_[64] = {};
    char warn_[64] = {};
};

}  // namespace ot
