// 手写 SkipList：单写多读。
//
// 为什么不用 std::map：
//   红黑树插入时的旋转会同时修改多个节点的多个指针，读者可能看到中间状态，
//   因此必须加锁。SkipList 的插入只改新节点自己的指针 + 前驱的 next 指针，
//   而已存在节点一旦创建就不再变化，读者可以完全无锁。
//
// 层高规则：p = 1/4 逐层上升，最大 kMaxHeight 层。
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

  // 要求：key 不在表中（重复 key 由上层保证不发生；MemTable 靠 seq 区分）
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

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
  // ---------------------------------------------------------------------------
  // TODO(1.2) RandomHeight
  // 返回 [1, kMaxHeight] 的整数，每层以 p = 1/4 的概率继续上升。
  // 不要用 std::rand()（线程不安全、质量差）。用无锁伪随机（xorshift）或
  // Arena 里存一个 std::mt19937。问自己：SkipList 只有单写线程，为什么
  // 仍然建议用 atomic 或每表独立的状态？（提示：多个 DB 实例可能在不同线程）
  // ---------------------------------------------------------------------------
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_(n->key, key) < 0);
  }

  // ---------------------------------------------------------------------------
  // TODO(1.3) FindGreaterOrEqual
  // 返回第一个 >= key 的节点（没有则 nullptr）。
  // prev != nullptr 时，把每层"最后一个严格小于 key 的节点"写进 prev[i]。
  // 骨架见正文 6.3 节。注意循环条件用 < key（不是 <=），否则插入位置会错。
  // ---------------------------------------------------------------------------
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  // ---------------------------------------------------------------------------
  // TODO(1.4) FindLast
  // 从最高层一路向右到底，下降，再向右到底，返回 level 0 的最后一个节点。
  // ---------------------------------------------------------------------------
  Node* FindLast() const;

  // 声明顺序决定初始化顺序：compare_ 与 arena_ 必须先于 head_，
  // 因为 head_ 的构造（NewNode）要使用 arena_。写成别的顺序是未定义行为。
  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
};

// ---------------------------------------------------------------------------
// Node：key + 变长 next 数组
// ---------------------------------------------------------------------------
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
    // release：保证 x 的内容（key 等）在指针可见前已经写完。
    // 这条内存序在 x86 上测不出差别，在 ARM 上是正确性必需的。
    next_[n].store(x, std::memory_order_release);
  }

  Node* NoBarrierNext(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }

 private:
  std::atomic<Node*> next_[1];  // 实际长度 = height，靠 placement new 扩容
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  Node* const node = new (mem) Node(key);
  for (int i = 0; i < height; ++i) {
    node->SetNext(i, nullptr);
  }
  return node;
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), max_height_(1) {
  for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
}

// ---------------------------------------------------------------------------
// TODO(1.2) 实现放在这里
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // 占位实现：永远返回 1，退化成链表（功能正确但复杂度 O(n)）。
  // 换成 p = 1/4 的正确实现后，测试性能相关断言才有意义。
  return 1;
}

// ---------------------------------------------------------------------------
// TODO(1.3) 实现放在这里
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  // 占位实现：只沿 level 0 线性扫描（能工作，但 O(n)）。
  // 正确实现要从 max_height()-1 层开始，逐层向右再下降。
  Node* x = head_;
  Node* next = x->Next(0);
  while (next != nullptr && compare_(next->key, key) < 0) {
    x = next;
    next = x->Next(0);
  }
  if (prev != nullptr) {
    for (int i = 0; i < kMaxHeight; ++i) prev[i] = x;
  }
  return next;
}

// ---------------------------------------------------------------------------
// TODO(1.4) 实现放在这里
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  Node* next = x->Next(0);
  while (next != nullptr) {
    x = next;
    next = x->Next(0);
  }
  return (x == head_) ? nullptr : x;
}

// ---------------------------------------------------------------------------
// TODO(1.5) Insert
//
// 输入：key（保证不与已有 key 相等）
// 输出：无
// 约束：
//   1. 用 FindGreaterOrEqual(key, prev) 拿到每层的前驱
//   2. prev[0] == nullptr 说明 key 已存在 -> 直接返回（不插入重复）
//   3. 用 RandomHeight() 决定高度 height
//   4. height > max_height_ 时，把更高的层的前驱补成 head_，并更新 max_height_
//   5. 新节点的 next[i] 指向 prev[i]->Next(i)，再让 prev[i]->SetNext(i, node)
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* const next = FindGreaterOrEqual(key, prev);
  (void)next;
  (void)prev;
  // 占位实现：什么都不插入，测试会失败。
  // 补完上面五步后删掉这一行。
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* const x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace lsm
