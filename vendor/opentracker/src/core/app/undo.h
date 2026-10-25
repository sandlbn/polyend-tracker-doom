// Pattern undo/redo ring (spec 05 §5.1 (a)) and pattern-file undo (§5.1 (b)).
// Snapshot storage is caller-provided (SDRAM on the device). 2.x: snapshots
// are whole 16-track working patterns (docs/M1_PLAN.md §1).
#pragma once

#include <cstdint>

#include "model/pattern.h"

namespace ot {

class UndoRing {
public:
    // slots >= 2; depth is limited to slots-1 (spec: 20 slots, depth 19).
    void attach(Pattern* slots, uint8_t* numbers, int count) {
        slots_ = slots;
        nums_ = numbers;
        n_ = count;
        clear();
    }
    void clear() { base_ = pos_ = redo_ = 0; }
    // Snapshot `cur` (pattern number num) before an edit; clears redo.
    void push(const Pattern& cur, uint8_t num);
    // Undo: saves `cur` for redo and overwrites it with the previous snapshot.
    // Returns false when there is nothing to undo. *num receives the pattern number.
    bool undo(Pattern& cur, uint8_t curNum, uint8_t* num);
    bool redo(Pattern& cur, uint8_t curNum, uint8_t* num);
    int depth() const { return pos_; }
    int redoCount() const { return redo_; }

private:
    int idx(int k) const { return (base_ + k) % n_; }
    Pattern* slots_ = nullptr;
    uint8_t* nums_ = nullptr;
    int n_ = 0;
    int base_ = 0, pos_ = 0, redo_ = 0;
};

// Simple LIFO of (pattern number, pattern image) used before song-screen
// pattern-file jobs (spec 06 §2.3 F3).
class FileUndo {
public:
    void attach(Pattern* slots, uint8_t* numbers, int count) {
        slots_ = slots;
        nums_ = numbers;
        n_ = count;
        top_ = count_ = 0;
    }
    void push(uint8_t num, const Pattern& p);
    bool pop(uint8_t* num, Pattern* out);
    // In-place variants (no 12 KB pattern copy on the stack): pushSlot()
    // reserves the next entry for pattern num and returns it for the caller
    // to fill; popSlot() removes the top entry and returns it (valid until
    // the next push). nullptr when not attached / empty.
    Pattern* pushSlot(uint8_t num);
    const Pattern* popSlot(uint8_t* num);
    int count() const { return count_; }
    int topNumber() const { return count_ ? nums_[(top_ + n_ - 1) % n_] : -1; }

private:
    Pattern* slots_ = nullptr;
    uint8_t* nums_ = nullptr;
    int n_ = 0, top_ = 0, count_ = 0;
};

}  // namespace ot
