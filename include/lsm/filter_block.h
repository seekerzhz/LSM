// filter block：把「每个 data block 的 Bloom 位图」串成一块。
//
// 为什么不给整个 SST 建一个 filter：那样任意一次查询都要检查全表，
// 位图很大、假阳性也随 key 数上升。分段之后，查询时只看「目标 key 可能
// 落在那几个 data block」对应的那几段，其余整段跳过。
//
// 分段粒度用块的**文件偏移**来定，不是块序号——写入时我们只知道偏移，
// 不知道序号，而读取时索引里存的正是偏移，两边自然对得上。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lsm/filter_policy.h"
#include "lsm/slice.h"

namespace lsm {

// 块偏移 >> 11 决定它属于哪一段，即每 2KB 数据一段 filter。
// 2KB 而不是 4KB：一个 4KB 的块正好落在段边界附近时，多分一段能避免
// 「一个块把所有相邻 key 的 filter 都拖进同一个位图」。
static constexpr int kFilterBaseLg = 11;
static constexpr uint64_t kFilterBase = 1ull << kFilterBaseLg;

class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* policy);
  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  // 宣告「接下来这个偏移开始的块属于新的一段」。多次调用且偏移跨过
  // 多段时，中间的段会生成空 filter（必须占位，否则索引错位）。
  void StartBlock(uint64_t block_offset);

  void AddKey(const Slice& key);

  // 生成 filter block 的字节：所有位图 + 偏移数组 + 偏移数组起点 + base_lg
  Slice Finish();

  bool empty() const { return start_.empty(); }

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  std::string keys_;                 // 所有 key 顺序拼在一起
  std::vector<uint32_t> start_;      // 每个 key 在 keys_ 里的起点
  std::string result_;               // 各段位图依次排列
  std::vector<uint32_t> filter_offsets_;  // 每段位图在 result_ 里的起点
  uint64_t filter_index_ = 0;        // 已经生成到第几段
};

class FilterBlockReader {
 public:
  // contents 会被**拷贝**一份。看起来浪费，但 filter block 只有几 KB，
  // 而用 Slice 会引入悬垂风险：调用方几乎总是传一个局部缓冲区进来。
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);

  // block_offset 是 data block 在文件里的偏移。
  // 返回 false 表示「这个块里一定没有这个 key」，可以整块跳过。
  bool KeyMayMatch(uint64_t block_offset, const Slice& key) const;

  size_t num_filters() const { return num_; }
  bool valid() const { return valid_; }

 private:
  const FilterPolicy* policy_;
  std::string contents_;
  const char* offsets_ = nullptr;  // 偏移数组起点（指向 contents_）
  size_t num_ = 0;                 // 段数
  size_t data_limit_ = 0;          // 位图区的结束位置（= 偏移数组起点）
  size_t base_lg_ = kFilterBaseLg;
  bool valid_ = false;
};

}  // namespace lsm
