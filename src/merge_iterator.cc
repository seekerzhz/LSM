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
  // 占位实现：总是选第 0 条有效路径，不做真正的最小值选择。
  // 补完 TODO(2.10) 后，这里应该遍历所有 children_ 取 key 最小者。
  for (size_t i = 0; i < children_.size(); ++i) {
    valid_[i] = children_[i]->Valid() ? 1 : 0;
    if (valid_[i]) {
      current_ = static_cast<int>(i);
      return;
    }
  }
  current_ = -1;
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
  // 同 Block::Iter::Next：骨架里用 if 而不是 assert，免得一个断言
  // 把整个测试进程 abort 掉，红榜看不到全貌。
  if (!Valid()) return;
  children_[current_]->Next();
  FindSmallest();
}

void MergingIterator::Prev() {
  if (!Valid()) return;
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
