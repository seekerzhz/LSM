// BlockBuilder：把一个有序的 key/value 流编码成一个 data block。
//
// 编码的核心是「前缀压缩」：后一条 key 只存与上一条的公共前缀之外的部分。
// 这让 10 万个有序 key 的块体积能降到 1/3 左右，代价是**随机访问失效**——
// 你想读第 i 条，必须先顺序解析前 i-1 条。
//
// 重启点就是为此存在的：每隔 kRestartInterval 条强制存一次完整 key
// （shared = 0），于是「从最近的的完整 key 开始往后扫最多 16 条」就能
// 定位到任何一条。这是「压缩率」和「随机访问」之间的折中参数。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/dbformat.h"
#include "lsm/slice.h"

namespace lsm {

// 每两条完整 key 之间隔多少条。
// 16 是 LevelDB 的经验值：调小 -> 块内 Seek 更快但块更大；
// 调大 -> 压缩率更好但最多要顺序扫更多条。
static constexpr int kRestartInterval = 16;

class BlockBuilder {
 public:
  // comparator 为 nullptr 时按字节序比较（metaindex 这类普通字符串 key 用它）。
  //
  // **必须传对的比较器**：internal key 的字节序和语义序不一致。
  // seq 存在尾部 8 字节里、小端编码，于是 seq=300 的串比 seq=100 的串
  // 「小」还是「大」取决于低位字节，并不单调。用 memcmp 做有序性检查
  // 会在「同一个 key 的多个版本」上误报，也会让块内二分的判据与真实顺序错位。
  explicit BlockBuilder(const InternalKeyComparator* comparator = nullptr)
      : comparator_(comparator) {
    Reset();
  }

  // 清空成「只剩一个重启点 0」的初始状态
  void Reset();

  // 追加一条。要求 key >= 上一次 Add 的 key。
  void Add(const Slice& key, const Slice& value);

  // 收尾：写重启点数组和重启点个数，返回整块的字节。
  // 返回的 Slice 指向本对象内部，Finish 之后调用 Reset 才失效。
  Slice Finish();

  // 当前（未 Finish 的）块大小估算，用来判断「是否该切下一个块」。
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }
  size_t NumEntries() const { return num_entries_; }
  size_t NumRestarts() const { return restarts_.size(); }

 private:
  int Compare(const Slice& a, const Slice& b) const {
    return comparator_ != nullptr ? (*comparator_)(a, b) : a.compare(b);
  }

  const InternalKeyComparator* comparator_ = nullptr;
  std::string buffer_;             // entries 的字节流
  std::vector<uint32_t> restarts_;  // 每个重启点在 buffer_ 里的偏移
  int counter_;                    // 距上一个重启点多少条了
  bool finished_;
  std::string last_key_;
  size_t num_entries_ = 0;
};

}  // namespace lsm
