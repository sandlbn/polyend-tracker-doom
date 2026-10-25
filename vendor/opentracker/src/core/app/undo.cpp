#include "app/undo.h"

namespace ot {

void UndoRing::push(const Pattern& cur, uint8_t num) {
    if (!slots_ || n_ < 2) return;
    slots_[idx(pos_)] = cur;
    nums_[idx(pos_)] = num;
    if (pos_ < n_ - 1) ++pos_;
    else base_ = (base_ + 1) % n_;
    redo_ = 0;
}

bool UndoRing::undo(Pattern& cur, uint8_t curNum, uint8_t* num) {
    if (!slots_ || pos_ == 0) return false;
    // Keep the current state as the redo point, then step back.
    slots_[idx(pos_)] = cur;
    nums_[idx(pos_)] = curNum;
    --pos_;
    cur = slots_[idx(pos_)];
    if (num) *num = nums_[idx(pos_)];
    ++redo_;   // invariant pos_ + redo_ <= n_ - 1 is preserved
    return true;
}

bool UndoRing::redo(Pattern& cur, uint8_t curNum, uint8_t* num) {
    (void)curNum;
    if (!slots_ || redo_ == 0) return false;
    ++pos_;
    --redo_;
    cur = slots_[idx(pos_)];
    if (num) *num = nums_[idx(pos_)];
    return true;
}

Pattern* FileUndo::pushSlot(uint8_t num) {
    if (!slots_ || n_ == 0) return nullptr;
    Pattern* p = &slots_[top_];
    nums_[top_] = num;
    top_ = (top_ + 1) % n_;
    if (count_ < n_) ++count_;
    return p;
}

const Pattern* FileUndo::popSlot(uint8_t* num) {
    if (!slots_ || count_ == 0) return nullptr;
    top_ = (top_ + n_ - 1) % n_;
    --count_;
    if (num) *num = nums_[top_];
    return &slots_[top_];
}

void FileUndo::push(uint8_t num, const Pattern& p) {
    if (Pattern* slot = pushSlot(num)) *slot = p;
}

bool FileUndo::pop(uint8_t* num, Pattern* out) {
    const Pattern* p = popSlot(num);
    if (!p) return false;
    if (out) *out = *p;
    return true;
}

}  // namespace ot
