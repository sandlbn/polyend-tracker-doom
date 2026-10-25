// File > Import Mod: ProTracker MOD import (spec 11 §1.1, §1.2) plus the
// pieces shared with the IT importer (it_import.h) and exporter.
//
// Container layout follows the public ProTracker "mod-form" description.
// The importer is a cooperative state machine: begin() only parses and
// validates (nothing is changed yet, so a refused file leaves the project
// alone); apply() starts writing into the target; each step() then does one
// bounded unit (one pattern, or one chunk of sample data), like the stock
// sub-op 0x13 (spec 11 §0.1).
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "model/instrument.h"
#include "model/pattern.h"
#include "model/project.h"

namespace ot {

// Where an import writes: the App model on the device, plain arrays in tests.
struct ModuleTarget {
    Project* project = nullptr;
    Instrument* instruments = nullptr;   // kSampleInstrumentCount entries
    bool* present = nullptr;             // kSampleInstrumentCount entries
    // Writable planar int16 storage for `frames` (x2 when stereo) of a
    // sample instrument; nullptr when the sample memory is full. Slots are
    // reserved in increasing order, one at a time.
    int16_t* (*reserve)(void* ctx, int slot, uint32_t frames, bool stereo) = nullptr;
    // Pattern number 1..255; the importer overwrites it completely.
    PatternStock* (*pattern)(void* ctx, int number) = nullptr;
    void* ctx = nullptr;
};

enum class ModuleKind : uint8_t { None = 0, Mod = 1, It = 2 };
enum class ImportStep : int8_t { Failed = -1, Done = 0, More = 1 };

// spec 11 §1.1 list filter 5: lower-cased name *contains* ".mod" or ".it".
bool moduleListFilter(const char* name);
// spec 11 §1.1: chosen by extension only (".mod" first, then ".it").
ModuleKind moduleKind(const char* name);
// spec 11 §1.1 step 3: project name = entry name up to the first '.', max 32.
void moduleProjectName(const char* fileName, char* out, uint32_t cap);

namespace modfmt {
constexpr int kMaxModChannels = 8;   // sample voices: tracks 1..8 (spec 03 §3.6)
constexpr int kDefaultSpeed = 6;
constexpr int kDefaultTempo = 125;

// Exact reads at an absolute offset (loops over short reads). Bytes past
// the end of the file read as 0; false on an I/O error. *got = real bytes.
bool readAt(IStorage& fs, IStorage::Handle h, uint32_t off, void* dst, uint32_t len, uint32_t* got = nullptr);

// Amiga period -> note (0..127), -1 for none. ProTracker C-2 (428) = 31:
// the sample is converted to 16 bit at full length, so the note is the
// period-table index + 7 (spec 11 §1.2 step 5 "converts properly").
// Non-table (finetuned / extended) periods round to the nearest semitone.
int periodToNote(uint16_t period);
// MOD finetune nibble (signed, 1/8 semitone) -> instrument finetune cents.
int8_t finetuneCents(uint8_t nibble);
// Tracker BPM of a tracker speed (ticks/row) and tempo: 4 rows per beat
// (one step = 15 s / BPM, spec 03 §1.2/§1.4), so BPM = 6 * tempo / speed.
float trackerBpm(int speed, int tempo);
// Tempo FX value (BPM = 2v, spec 03 §3.6), 5..200 (never 4 = STOP).
uint8_t tempoFxValue(float bpm);
// Project tempo range used by the importers (spec 03 §1.2 ".it import clamp").
float clampProjectTempo(float bpm);
// Position FX (0..100 % of the sample) for a sample offset in frames.
uint8_t positionFxValue(uint32_t offsetFrames, uint32_t lengthFrames);
// Fixed-field name -> printable instrument name, "-" when empty.
void moduleName(const uint8_t* src, uint32_t len, char* dst, uint32_t cap);

// Speed / tempo while patterns are converted in song order.
struct TempoState {
    int speed = kDefaultSpeed;
    int tempo = kDefaultTempo;
    float bpm() const { return trackerBpm(speed, tempo); }
};
}  // namespace modfmt

class ModImport {
public:
    // Parse and validate. On failure error() says why; nothing was changed.
    // `scratch` (>= kScratchBytes) holds the I/O buffers until the end.
    static constexpr uint32_t kScratchBytes = 0x1000;
    bool begin(IStorage& fs, const char* path, uint8_t* scratch, uint32_t scratchBytes);
    // Start writing into `t` (the caller has created the new project).
    void apply(const ModuleTarget& t);
    ImportStep step(uint32_t& cost);
    int percent() const;
    void abort();
    const char* error() const { return err_; }
    // Non-fatal problem (sample memory full, truncated file), or "".
    const char* warning() const { return warn_; }

    int channels() const { return ch_; }
    int sampleCount() const { return nSmp_; }
    int patternCount() const { return nPat_; }

private:
    struct Smp {
        uint32_t words;
        uint16_t repStart, repLen;
        uint8_t vol;
        uint8_t fine;
    };
    enum Stage : uint8_t { kIdle, kPatterns, kSamples, kDone };
    bool nextPattern(int& p);
    void convertPattern(PatternStock& pat, bool first);
    void finish();

    IStorage* fs_ = nullptr;
    IStorage::Handle h_ = -1;
    ModuleTarget t_;
    uint8_t* buf_ = nullptr;          // scratch: one pattern (<= 64*8*4 bytes) or a sample chunk
    uint32_t fileSize_ = 0;
    char name_[32] = {};
    uint8_t smpHdr_[31][30] = {};
    Smp smp_[31] = {};
    uint8_t orders_[128] = {};
    uint8_t done_[16] = {};           // pattern-converted bitmap
    uint8_t lastInstr_[modfmt::kMaxModChannels] = {};
    int nSmp_ = 31, ch_ = 4, songLen_ = 1, nPat_ = 1;
    uint32_t patOff_ = 0, dataOff_ = 0;
    Stage stage_ = kIdle;
    int seqPos_ = 0, rest_ = 0, smpIdx_ = 0;
    uint32_t smpPos_ = 0, smpOff_ = 0;
    int16_t* dst_ = nullptr;
    bool memFull_ = false, projTempoSet_ = false;
    float projBpm_ = 0;
    modfmt::TempoState ts_;
    uint32_t units_ = 0, total_ = 1;
    char err_[64] = {};
    char warn_[64] = {};
};

}  // namespace ot
