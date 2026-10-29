// SST 文件的「骨架」类型：BlockHandle 与 Footer。
//
// 这两个东西解决的问题是：一个 SST 由多个块拼起来，读的时候必须先知道
// 「某个块在文件的哪个位置、有多长」。BlockHandle 就是这个二元组，
// Footer 则固定在文件最后 48 字节，负责把「元数据块在哪」告诉读者。
//
// 为什么 Footer 要定长：读它的时候你还不知道文件里有什么，你只知道文件
// 多长。定长意味着「从末尾倒数 48 字节」永远是对的，一次 pread 就能拿到
// 所有入口。变长的 footer 需要先读一个长度，而那个长度本身又在哪里呢？
#pragma once

#include <cstdint>
#include <string>

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// 文件最后 8 字节。作用是「识别这是不是一个 SST」。
// 少了它，一个被截断的文件会让你把随机字节当 offset 去解析，
// 崩溃点离真正的现场几百行。
static constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 变长编码后的 handle 最长能占多少字节。两个 VarInt64 最多 20 字节，
// 这里取 20 是为了给 footer 里的定长布局留位置。
static constexpr size_t kMaxEncodedHandleLength = 20;

// 20 + 20 + 8。Footer 定长，所以「打开 SST」只需要一次 48 字节读。
static constexpr size_t kFooterEncodedLength = 48;

// 一个块在文件里的位置。
class BlockHandle {
 public:
  BlockHandle() = default;
  BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  void set_offset(uint64_t v) { offset_ = v; }
  void set_size(uint64_t v) { size_ = v; }

  // 两个 VarInt64。索引块里每条索引项都带一个 handle，
  // 所以这里紧凑比定长重要：2748 个块，每条省 10 字节就是 27KB。
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

  std::string ToString() const;

 private:
  // 默认值取全 1，这样「忘了 set」的 handle 一编码就是一个荒谬的巨大 offset，
  // 读文件时立刻报错，而不是读到块 0 然后给出错误的查询结果。
  uint64_t offset_ = ~static_cast<uint64_t>(0);
  uint64_t size_ = ~static_cast<uint64_t>(0);
};

struct Footer {
  BlockHandle metaindex_handle;
  BlockHandle index_handle;

  // 写满 48 字节：两个 handle 的变长编码各补 0 到 20 字节，再追加 magic。
  void EncodeTo(std::string* dst) const;

  // 成功时把 input 消费完。magic 不对返回 Corruption。
  Status DecodeFrom(Slice* input);
};

}  // namespace lsm
