#include "model/pattern_store.h"

#include <atomic>
#include <cstring>

namespace ot {

void ExtTrackPool::attach(Track* blocks, int count, uint8_t* meta) {
    if (!blocks || !meta || count <= 0) {
        blocks_ = nullptr;
        index_ = nullptr;
        bits_ = nullptr;
        count_ = 0;
    } else {
        blocks_ = blocks;
        count_ = count > kMaxBlocks ? kMaxBlocks : count;
        index_ = reinterpret_cast<uint16_t*>(meta);
        bits_ = meta + indexBytes();
    }
    clear();
}

void ExtTrackPool::clear() {
    used_ = 0;
    hint_ = 0;
    fullEvents_ = 0;
    if (!blocks_) return;
    for (uint32_t i = 0; i < kIndexEntries; ++i) index_[i] = kNone;
    std::memset(bits_, 0, bitmapBytes(count_));
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

uint16_t ExtTrackPool::blockOf(int num, int e) const {
    if (!blocks_ || !valid(num, e)) return kNone;
    return index_[slot(num, e)];
}

Track* ExtTrackPool::find(int num, int e) {
    const uint16_t b = blockOf(num, e);
    return b == kNone ? nullptr : &blocks_[b];
}

const Track* ExtTrackPool::find(int num, int e) const {
    const uint16_t b = blockOf(num, e);
    return b == kNone ? nullptr : &blocks_[b];
}

Track* ExtTrackPool::alloc(int num, int e, const Track& init) {
    if (!valid(num, e)) return nullptr;
    if (Track* t = find(num, e)) return t;
    if (used_ >= count_) {
        ++fullEvents_;
        return nullptr;
    }
    int b = hint_;
    for (int k = 0; k < count_ && isUsed(b); ++k) b = b + 1 < count_ ? b + 1 : 0;
    if (isUsed(b)) {   // bitmap and counter disagree: treat as full, never overwrite
        ++fullEvents_;
        return nullptr;
    }
    bits_[b >> 3] = uint8_t(bits_[b >> 3] | (1u << (b & 7)));
    ++used_;
    hint_ = b + 1 < count_ ? b + 1 : 0;
    blocks_[b] = init;
    // Publish after the block holds its data (audio-context readers).
    std::atomic_signal_fence(std::memory_order_seq_cst);
    index_[slot(num, e)] = uint16_t(b);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return &blocks_[b];
}

void ExtTrackPool::release(int num, int e) {
    const uint16_t b = blockOf(num, e);
    if (b == kNone) return;
    index_[slot(num, e)] = kNone;   // unpublish first
    std::atomic_signal_fence(std::memory_order_seq_cst);
    if (isUsed(b)) {
        bits_[b >> 3] = uint8_t(bits_[b >> 3] & ~(1u << (b & 7)));
        --used_;
    }
}

void ExtTrackPool::releasePattern(int num) {
    for (int e = 0; e < kExtTrackCount; ++e) release(num, e);
}

}  // namespace ot
