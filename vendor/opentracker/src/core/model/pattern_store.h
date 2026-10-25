// Extension-track pool (2.x, docs/M1_PLAN.md §1): tracks 12..15 (13..16 in
// the UI) of the resident patterns. The stock tracks 0..11 of every pattern
// stay resident (App::storedStock, the .mtp image); an extension track only
// takes a 769-byte block while it holds data.
//
// Memory is caller-provided (SDRAM on the device): `blocks` track records
// plus the meta area (index + used bitmap, metaBytes()). The index has one
// entry per (pattern 1..255, ext track 0..3); kNone = no block.
//
// SPEC GAP (plan §1 says uint8_t[255][4], 0xFF = empty): a uint8_t entry can
// only name 255 blocks, the default pool has 384, so entries are uint16_t
// (2,040 bytes) with 0xFFFF = empty.
//
// Concurrency: all writes happen in the main loop. The audio context only
// reads (find() / copyTrack()). alloc() fills the block before it publishes
// the index entry, release() clears the entry before the block is reused,
// so an audio-block read that preempts either sees a consistent track.
#pragma once

#include <cstdint>

#include "model/constants.h"
#include "model/pattern.h"

namespace ot {

class ExtTrackPool {
public:
    static constexpr uint16_t kNone = 0xFFFF;
    static constexpr int kDefaultBlocks = 384;     // spec gap 9: 295,296 bytes
    static constexpr int kMaxBlocks = 0xFFFE;
    static constexpr uint32_t kIndexEntries = uint32_t(kPatternCount) * kExtTrackCount;
    static constexpr uint32_t indexBytes() { return kIndexEntries * sizeof(uint16_t); }
    static constexpr uint32_t bitmapBytes(int blocks) { return (uint32_t(blocks) + 7u) / 8u; }
    // Meta area: index (uint16_t, so 2-byte aligned) then the used bitmap.
    static constexpr uint32_t metaBytes(int blocks) { return indexBytes() + bitmapBytes(blocks); }
    static constexpr uint32_t blockBytes(int blocks) { return uint32_t(blocks) * sizeof(Track); }

    // `meta` must hold metaBytes(count) bytes, 2-byte aligned. Clears the pool.
    // A null pool (blocks == nullptr or count <= 0) is valid: every ext track
    // reads as empty and every allocation fails.
    void attach(Track* blocks, int count, uint8_t* meta);
    bool attached() const { return blocks_ != nullptr; }
    void clear();                                  // every block free, every index entry kNone

    int capacity() const { return count_; }
    int used() const { return used_; }
    int freeBlocks() const { return count_ - used_; }
    // Allocations refused because the pool was full (since attach/clear).
    uint32_t fullEvents() const { return fullEvents_; }

    // num 1..255, e 0..3 (track kStockTrackCount + e). nullptr = no block.
    Track* find(int num, int e);
    const Track* find(int num, int e) const;
    // Block of (num, e); a new block is initialised from `init`. nullptr when
    // the pool is full (counted in fullEvents()) or num/e is out of range.
    Track* alloc(int num, int e, const Track& init);
    void release(int num, int e);
    void releasePattern(int num);                  // all 4 ext tracks of num
    // Block number of (num, e) or kNone (tests).
    uint16_t blockOf(int num, int e) const;

private:
    static bool valid(int num, int e) { return num >= 1 && num <= kPatternCount && e >= 0 && e < kExtTrackCount; }
    static uint32_t slot(int num, int e) { return uint32_t(num - 1) * kExtTrackCount + uint32_t(e); }
    bool isUsed(int b) const { return (bits_[b >> 3] >> (b & 7)) & 1; }

    Track* blocks_ = nullptr;
    uint16_t* index_ = nullptr;
    uint8_t* bits_ = nullptr;
    int count_ = 0;
    int used_ = 0;
    int hint_ = 0;                                 // next block to try
    uint32_t fullEvents_ = 0;
};

}  // namespace ot
