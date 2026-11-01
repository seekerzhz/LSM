// 参考实现（教师版），仅用于验证骨架。
#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>

#include "lsm/arena.h"
#include "lsm/slice.h"

namespace lsm {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  explicit SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Insert(const Key& key);
  bool Contains(const Key& key) const;
  int MaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast() { node_ = list_->FindLast(); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_(n->key, key) < 0);
  }
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}
  Key const key;

  Node* Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }
  Node* NoBarrierNext(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  void NoBarrierSetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const mem =
      arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  Node* const node = new (mem) Node(key);
  for (int i = 0; i < height; ++i) node->NoBarrierSetNext(i, nullptr);
  return node;
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), max_height_(1) {
  for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  static thread_local uint32_t rnd = 0x9e3779b9u;
  int height = 1;
  while (height < kMaxHeight) {
    rnd ^= rnd << 13;
    rnd ^= rnd >> 17;
    rnd ^= rnd << 5;
    if ((rnd & 3) != 0) break;   // p = 1/4 继续上升
    ++height;
  }
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = MaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    while (next != nullptr && compare_(next->key, key) < 0) {
      x = next;
      next = x->Next(level);
    }
    if (prev != nullptr) prev[level] = x;
    if (level == 0) return next;
    --level;
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  int level = MaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    while (next != nullptr) {
      x = next;
      next = x->Next(level);
    }
    if (level == 0) break;
    --level;
  }
  return (x == head_) ? nullptr : x;
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  if (x != nullptr && Equal(key, x->key)) return;

  const int height = RandomHeight();
  if (height > MaxHeight()) {
    for (int i = MaxHeight(); i < height; ++i) prev[i] = head_;
    max_height_.store(height, std::memory_order_relaxed);
  }
  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* const x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace lsm
