// File > Export > "As .it" (spec 11 §2): Impulse Tracker module writer.
//
// Layout per the public ITTECH.TXT. All offsets are computed up front (the
// pattern sizes by a dry encode), so the file is written front to back;
// stock writes zeros and patches them (spec 11 §2.2). Cooperative: begin()
// opens the file, each step() writes one unit (header, one instrument, one
// sample header, one chunk of sample data, one pattern).
#pragma once

#include <cstdint>

#include "formats/mod_import.h"

namespace ot {

struct ItExportSource {
    const Project* project = nullptr;
    const Instrument* instruments = nullptr;   // kSampleInstrumentCount
    const bool* present = nullptr;             // kSampleInstrumentCount
    // Planar int16 sample data of a slot (left block, then right when
    // stereo); nullptr / 0 frames when empty.
    const int16_t* (*samples)(void* ctx, int slot, uint32_t* frames, bool* stereo) = nullptr;
    // Pattern 1..255, nullptr when it does not exist.
    const PatternStock* (*pattern)(void* ctx, int number) = nullptr;
    void* ctx = nullptr;
};

namespace itfmt {
constexpr int kExportInstruments = 48;   // spec 11 §2.2 InsNum / SmpNum
constexpr int kExportMaxPatterns = 200;  // IT pattern numbers 0..199
constexpr uint32_t kImpiSize = 554;
constexpr uint32_t kImpsSize = 80;
constexpr uint32_t kMaxPatternBytes = 128u * (12u * 7u + 1u) + 4u;

// Exported speed / tempo for a project tempo: speed 6 with tempo = BPM;
// above 255 BPM speed 3 with tempo = BPM / 2 (stock wraps the byte).
void exportSpeedTempo(float bpm, uint8_t& speed, uint8_t& tempo);
// Tracker volume (0..100, 50 = 0 dB, spec 04 §4.2) -> IT volume 0..64.
uint8_t itVolume(int v);
// Packed IT pattern data (without the 8-byte header) of `p`; returns bytes.
// restoreTempo != 0: a T<restoreTempo> on channel 13 of row 0 unless row 0
// sets a tempo itself.
uint32_t encodePattern(const PatternStock& p, const Instrument* ins, uint8_t speed, uint8_t restoreTempo, uint8_t* out);
// The 554-byte IMPI and 80-byte IMPS records (spec 11 §2.3, §2.4).
void buildInstrument(uint8_t* out, int index, const Instrument& in, uint8_t speed, uint8_t itTempo);
void buildSampleHeader(uint8_t* out, int index, const Instrument& in, uint32_t frames, bool stereo,
                       uint32_t dataOffset);
}  // namespace itfmt

class ItExport {
public:
    static constexpr uint32_t kScratchBytes = 0x4000;
    bool begin(IStorage& fs, const char* path, const ItExportSource& src, uint8_t* scratch, uint32_t scratchBytes);
    ImportStep step(uint32_t& cost);
    int percent() const;
    void abort();
    const char* error() const { return err_; }
    int patternCount() const { return patNum_; }
    int orderCount() const { return ordNum_; }

private:
    enum Stage : uint8_t { kIdle, kHeader, kInstr, kSmpHdr, kData, kPatterns, kDone };
    bool put(const void* p, uint32_t n);
    bool fail(const char* msg);

    IStorage* fs_ = nullptr;
    IStorage::Handle h_ = -1;
    ItExportSource src_;
    uint8_t* buf_ = nullptr;
    Stage stage_ = kIdle;
    int idx_ = 0;
    uint32_t pos_ = 0;
    uint16_t ordNum_ = 0, patNum_ = 0;
    uint8_t speed_ = 6, tempo_ = 125, restoreT_ = 0;
    uint32_t hdrSize_ = 0;
    uint32_t dataOff_[itfmt::kExportInstruments] = {};
    uint16_t patSize_[itfmt::kExportMaxPatterns] = {};
    uint32_t units_ = 0, total_ = 1;
    char err_[64] = {};
};

}  // namespace ot
