// Whole-file I/O over IStorage (spec 02 §0) and storage-level load/save
// helpers for each format plus SD-card path builders (spec 02 §1).
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "formats/instrument_pti.h"
#include "formats/pattern_mtp.h"
#include "formats/project_mt.h"
#include "hal_api.h"
#include "model/pattern.h"
#include "model/project.h"

namespace ot {

constexpr uint32_t kIoChunk = 0x7F80;  // spec 02 §0

// Zero-fills buf[requested], then reads min(fileSize, requested) bytes.
// Returns Ok, NotFound or IoError; *bytesRead receives the byte count.
FmtStatus readWholeFile(IStorage& fs, const char* path, uint8_t* buf, uint32_t requested,
                        uint32_t* bytesRead = nullptr);
// Deletes an existing target first, then creates and writes it in chunks.
FmtStatus writeWholeFile(IStorage& fs, const char* path, const uint8_t* buf, uint32_t len);

// Path builders (spec 02 §1). `base` is e.g. "Workspace" or "Projects/Song".
// Return false if the output did not fit.
bool projectFilePath(char* out, size_t cap, const char* base);
bool patternFilePath(char* out, size_t cap, const char* base, int patternNumber);    // 1..255
bool patternNamesPath(char* out, size_t cap, const char* base);
bool workspaceInstrumentPath(char* out, size_t cap, const char* base, int instrumentNumber);  // 1..48
bool savedInstrumentPath(char* out, size_t cap, const char* base, int instrumentNumber, const char* name);

// Storage-level wrappers around the codecs.
FmtStatus loadProjectFile(IStorage& fs, const char* path, Project& p, ProjectLoadInfo* info = nullptr);
FmtStatus saveProjectFile(IStorage& fs, const char* path, const Project& p);
// Pattern files (2.x): saved as the Tracker Mini 16-track layout (0x3030,
// spec 21 §4.1); any size loads through decodePattern16 (§4.2 upgrades).
// The PatternStock overloads drop / default tracks 12..15.
FmtStatus loadPatternFile(IStorage& fs, const char* path, PatternStock& p, const PatternUpgradeContext* ctx = nullptr,
                          PatternLoadInfo* info = nullptr);
FmtStatus loadPatternFile(IStorage& fs, const char* path, Pattern& p, const PatternUpgradeContext* ctx = nullptr,
                          PatternLoadInfo* info = nullptr);
FmtStatus savePatternFile(IStorage& fs, const char* path, const PatternStock& p);
FmtStatus savePatternFile(IStorage& fs, const char* path, const Pattern& p);
// Missing file -> a zeroed (all names empty) result and NotFound; the caller
// may create it (spec 02 §5).
FmtStatus loadPatternNamesFile(IStorage& fs, const char* path, PatternNames& n);
FmtStatus savePatternNamesFile(IStorage& fs, const char* path, const PatternNames& n);
FmtStatus loadRecentsFile(IStorage& fs, const char* path, Recents& r);
FmtStatus saveRecentsFile(IStorage& fs, const char* path, const Recents& r);

// Instrument load: header + payload into `samples` (planar: left block, then
// right block when stereo). layoutOut reports headerDirty (spec 02 §6.3).
FmtStatus loadInstrumentFile(IStorage& fs, const char* path, Instrument& ins, int16_t* samples,
                             size_t capSamples, bool stereoEnabled, PtiSampleLayout* layoutOut = nullptr);
// Writes header block + payload (planar). samples may be null when lengthFrames == 0.
FmtStatus saveInstrumentFile(IStorage& fs, const char* path, const Instrument& ins, const int16_t* samples);
// Rewrite only the 0x188-byte header block of an existing file (autosave,
// spec 02 §1.1), leaving the sample payload in place.
FmtStatus saveInstrumentHeader(IStorage& fs, const char* path, const Instrument& ins);

}  // namespace ot
