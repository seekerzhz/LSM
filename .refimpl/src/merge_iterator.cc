// 参考实现（教师版），仅用于验证骨架。
#include "lsm/merge_iterator.h"

#include <cassert>

namespace lsm {

MergingIterator::MergingIterator(const InternalKeyComparator* comparator,
                                 std::vector<Iterator*>* children)
    : comparator_(comparator),
      children_(*children),
      current_(-1),
      direction_forward_(true) {
  valid_.assign(children_.size(), 0);
}

MergingIterator::~MergingIterator() {
  for (Iterator* it : children_) delete it;
}

bool MergingIterator::Valid() const { return current_ >= 0; }

void MergingIterator::FindSmallest() {
  current_ = -1;
  for (size_t i = 0; i < children_.size(); ++i) {
    valid_[i] = children_[i]->Valid() ? 1 : 0;
    if (!valid_[i]) continue;
    if (current_ < 0 ||
        (*comparator_)(children_[i]->key(), children_[static_cast<size_t>(current_)]->key()) < 0) {
      current_ = static_cast<int>(i);
    }
  }
}

void MergingIterator::SeekToFirst() {
  for (Iterator* it : children_) it->SeekToFirst();
  direction_forward_ = true;
  FindSmallest();
}

void MergingIterator::SeekToLast() {
  for (Iterator* it : children_) it->SeekToLast();
  direction_forward_ = false;
  FindSmallest();
}

void MergingIterator::Seek(const Slice& target) {
  for (Iterator* it : children_) it->Seek(target);
  direction_forward_ = true;
  FindSmallest();
}

void MergingIterator::Next() {
  assert(Valid());
  children_[current_]->Next();
  FindSmallest();
}

void MergingIterator::Prev() {
  assert(Valid());
  children_[current_]->Prev();
  FindSmallest();
}

Slice MergingIterator::key() const {
  assert(Valid());
  return children_[current_]->key();
}

Slice MergingIterator::value() const {
  assert(Valid());
  return children_[current_]->value();
}

Status MergingIterator::status() const {
  for (Iterator* it : children_) {
    const Status s = it->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace lsm
