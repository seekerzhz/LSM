#include "lsm/block.h"

#include <cassert>
#include <cstring>

#include "lsm/coding.h"

namespace lsm {
namespace {

// 解析一条 entry 的三个 VarInt32，返回 key 后缀的起点。
// 返回 nullptr 表示「越界或编码坏了」——注意这个检查不能省：
// 一个损坏的块里 non_shared + value_length 可以宣称任意大的长度，
// 不校验就会让 Slice 指向块外的内存。
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                        uint32_t* non_shared, uint32_t* value_length) {
  Slice input(p, static_cast<size_t>(limit - p));
  if (!GetVarint32(&input, shared)) return nullptr;
  if (!GetVarint32(&input, non_shared)) return nullptr;
  if (!GetVarint32(&input, value_length)) return nullptr;
  const size_t rest = static_cast<size_t>(limit - input.data());
  if (rest < static_cast<size_t>(*non_shared) + static_cast<size_t>(*value_length)) {
    return nullptr;
  }
  return input.data();
}

}  // namespace

Block::Block(std::string contents) : data_(std::move(contents)) {
  size_ = data_.size();
  if (size_ < 4) {
    status_ = Status::Corruption("block too short");
    return;
  }
  num_restarts_ = DecodeFixed32(data_.data() + size_ - 4);
  // 不校验的话，restarts_offset_ = size_ - 4 - n*4 在 n 很大时会无符号下溢，
  // 变成一个天文数字，后面所有指针运算全部越界。
  if (num_restarts_ > (size_ - 4) / 4) {
    status_ = Status::Corruption("bad restart count");
    num_restarts_ = 0;
    return;
  }
  restarts_offset_ = size_ - 4 - static_cast<size_t>(num_restarts_) * 4;
}

// ---------------------------------------------------------------------------
// Block::Iter
//
// 这个类的全部复杂度来自一个事实：entry 是**变长**的，没有「下一条在哪」
// 的指针。所以：
//   Next  = 从当前 entry 末尾继续解析一条        O(1)
//   Prev  = 回到最近的完整 key（重启点）正向扫     O(kRestartInterval)
//   Seek  = 重启点数组上二分 + 组内顺序扫 16 条    O(log R + 16)
//
// Prev 慢一个数量级是整个设计里最明显的不对称，这是有意接受的：
// 读路径上 Next 用得多（Scan 向前），Prev 几乎不用。
// ---------------------------------------------------------------------------
class Block::Iter : public Iterator {
 public:
  Iter(const InternalKeyComparator* comparator, const Block* block)
      : comparator_(comparator),
        block_(block),
        data_(block->data_.data()),
        restarts_(block->data_.data() + block->restarts_offset_),
        num_restarts_(block->num_restarts_),
        current_(static_cast<uint32_t>(block->restarts_offset_)),
        restart_index_(num_restarts_ > 0 ? num_restarts_ - 1 : 0) {
    if (block->IsCorrupted()) status_ = block->status();
  }

  bool Valid() const override { return current_ < block_->restarts_offset_; }

  void SeekToFirst() override {
    // 损坏的块 num_restarts_ 会是 0，此时连 GetRestartPoint(0) 都不合法
    if (num_restarts_ == 0) return;
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    if (num_restarts_ == 0) return;
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < block_->restarts_offset_) {
      // 一直走到块尾，最后一条就是我们要的
    }
  }

  // -------------------------------------------------------------------------
  // TODO(2.2) Block::Iter::Seek
  //
  // 输入：target
  // 输出：定位到第一个 key >= target；没有则 Valid() == false
  // 约束：必须「重启点数组上二分 + 组内线性扫」，不能从头线性扫。
  //       二分的目标是「最后一个首 key < target 的重启点」，然后从那里
  //       往后解析最多 kRestartInterval 条。组内第一条的 shared 一定是 0，
  //       所以可以从它的偏移独立开始解析。
  // -------------------------------------------------------------------------
  void Seek(const Slice& target) override {
    if (num_restarts_ == 0) return;

    // 二分：找最后一个「首 key < target」的重启点。
    // 用 left/right 收敛而不是 std::lower_bound，因为我们要的是「上界的前一个」，
    // 而重启点的 key 只能顺序解析出来（不是数组），给不了随机访问迭代器。
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = left + (right - left + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, restarts_,
                                       &shared, &non_shared, &value_length);
      // 重启点处的 entry 必须能独立解析：shared 不为 0 说明块已经坏了
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      const Slice mid_key(key_ptr, non_shared);
      if (Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (Compare(key_, target) >= 0) return;
    }
  }

  void Next() override {
    // Valid() 为假时调用 Next() 在真实代码里是调用方的错。但在本课程的骨架里
    // 它几乎总是意味着另一件事：SeekToFirst() 还没写，迭代器停在
    // restarts_offset_ 上，于是第一次 Next() 就在这里 abort——
    // 报错信息看不出跟 SeekToFirst 有什么关系，所以写在这里。
    assert(Valid() && "Block::Iter::Next() 在无效位置上被调用：先确认 SeekToFirst() 真的定位了");
    ParseNextKey();
  }

  // -------------------------------------------------------------------------
  // TODO(2.3) Block::Iter::Next / Prev
  //
  // Next：解析下一条即可。
  // Prev：变长编码没有反向指针，只能「回到当前所在的 restarts 组，
  //       从那里正向解析到目标的前一条」。所以 Prev 比 Next 慢一个数量级。
  //       注意 restart_index_ 要往回退，直到「该重启点的偏移 < 当前偏移」。
  // -------------------------------------------------------------------------
  void Prev() override {
    if (!Valid()) return;
    const uint32_t original = current_;

    // 先退到「位于当前 entry 之前」的那个重启点
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // 已经在第一条之前了
        current_ = static_cast<uint32_t>(block_->restarts_offset_);
        restart_index_ = num_restarts_ > 0 ? num_restarts_ - 1 : 0;
        key_.clear();
        value_ = Slice();
        return;
      }
      --restart_index_;
    }

    SeekToRestartPoint(restart_index_);
    // 从那个重启点正向走，停在 original 的前一条
    do {
      // 循环体为空：ParseNextKey 会推进 current_，
      // 条件里的 NextEntryOffset() < original 判断「下一条还没到原位置」
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  Slice key() const override {
    assert(Valid());
    return Slice(key_);
  }

  Slice value() const override {
    assert(Valid());
    return value_;
  }

  Status status() const override {
    return status_.ok() ? block_->status() : status_;
  }

 private:
  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32(restarts_ + index * 4);
  }

  // 把迭代器状态重置到「某个重启点之前」：key_ 清空、value_ 指向那个偏移
  // 且长度为 0，于是 NextEntryOffset() 恰好等于该偏移。
  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    const uint32_t offset = GetRestartPoint(index);
    value_ = Slice(data_ + offset, 0);
    restart_index_ = index;
    current_ = offset;
  }

  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + block_->restarts_offset_;
    if (p >= limit) {
      // 走到 entries 区末尾（后面是重启点数组），迭代结束
      current_ = static_cast<uint32_t>(block_->restarts_offset_);
      key_.clear();
      value_ = Slice();
      return false;
    }

    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    // key_.size() < shared 说明「共享前缀比上一条 key 还长」，一定是坏了
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }

    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = Slice(p + non_shared, value_length);

    // 顺着往前推进 restart_index_，让 Prev 能直接用
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  void CorruptionError() {
    current_ = static_cast<uint32_t>(block_->restarts_offset_);
    restart_index_ = num_restarts_ > 0 ? num_restarts_ - 1 : 0;
    key_.clear();
    value_ = Slice();
    status_ = Status::Corruption("bad entry in block");
  }

  int Compare(const Slice& a, const Slice& b) const {
    // data/index 块里存的是 internal key，用 comparator_ 比较才严谨：
    // 直接 memcmp 在「本课程的编码恰好让字节序等于语义序」时也对，
    // 但那是依赖编码细节，改一个字节就静默出错。
    return comparator_ != nullptr ? (*comparator_)(a, b) : a.compare(b);
  }

  const InternalKeyComparator* comparator_;
  const Block* block_;
  const char* data_;
  const char* restarts_;
  uint32_t num_restarts_;
  uint32_t current_;        // 当前 entry 的起始偏移；== restarts_offset_ 即结束
  uint32_t restart_index_;
  std::string key_;
  Slice value_;
  Status status_;
};

Iterator* Block::NewIterator(const InternalKeyComparator* comparator) const {
  return new Iter(comparator, this);
}

size_t Block::CountEntries() const {
  if (IsCorrupted() || num_restarts_ == 0) return 0;
  size_t n = 0;
  const char* p = data_.data();
  const char* limit = data_.data() + restarts_offset_;
  while (p < limit) {
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    const char* payload = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (payload == nullptr) return n;
    p = payload + non_shared + value_length;
    ++n;
  }
  return n;
}

}  // namespace lsm
