// Arena: 简单的 bump allocator。
//
// 为什么需要它：MemTable 里每个节点都要分配一次，如果每个都是 malloc，
// 一是慢（每次都要走分配器），二是内存开销大（每个块有 16 字节头部），
// 三是释放时要逐个 free。
// Arena 的做法是：一次申请 4KB 的大块，从中切给调用者；析构时只释放大块。
// 代价：不能单独释放一个小对象（对 MemTable 无所谓，整表一起丢）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsm {

class Arena {
 public:
  Arena() = default;
  ~Arena() { ReleaseAll(); }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // 返回一块至少 bytes 字节、按 8 字节对齐的内存。
  char* Allocate(size_t bytes) { return AllocateAligned(bytes); }

  // 按 8 字节对齐分配。
  char* AllocateAligned(size_t bytes) {
    const size_t align = 8;
    size_t need = (bytes + align - 1) & ~(align - 1);
    if (need == 0) need = align;

    if (cur_ != nullptr && static_cast<size_t>(end_ - cur_) >= need) {
      char* result = cur_;
      cur_ += need;
      return result;
    }
    return AllocateFallback(need);
  }

  // 已分配的总字节数（用于 ApproximateMemoryUsage）
  size_t MemoryUsage() const {
    size_t total = 0;
    for (const Block& b : blocks_) total += b.size;
    return total + static_cast<size_t>(cur_ != nullptr ? (end_ - cur_) : 0);
  }

 private:
  struct Block {
    char* data;
    size_t size;
  };

  char* AllocateFallback(size_t need) {
    // 大对象单独一个块，避免浪费
    if (need > kBlockSize / 4) {
      char* p = new char[need];
      blocks_.push_back(Block{p, need});
      return p;
    }
    char* p = new char[kBlockSize];
    blocks_.push_back(Block{p, kBlockSize});
    cur_ = p + need;
    end_ = p + kBlockSize;
    return p;
  }

  void ReleaseAll() {
    for (const Block& b : blocks_) delete[] b.data;
    blocks_.clear();
    cur_ = nullptr;
    end_ = nullptr;
  }

  static constexpr size_t kBlockSize = 4096;
  std::vector<Block> blocks_;
  char* cur_ = nullptr;
  char* end_ = nullptr;
};

}  // namespace lsm
