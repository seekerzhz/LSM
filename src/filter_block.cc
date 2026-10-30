#include "lsm/filter_block.h"

#include <cassert>

#include "lsm/coding.h"

namespace lsm {

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}

// ---------------------------------------------------------------------------
// TODO(2.13) FilterBlockBuilder 的三个方法
//
// StartBlock(block_offset)：
//   filter_index = block_offset / kFilterBase
//   当 filter_index > filter_index_ 时反复 GenerateFilter() 并 ++filter_index_。
//   循环（而不是 if）是必须的：相邻两个块可能隔着好几段（比如中间的空隙），
//   那些段必须各生成一个空 filter 占位，否则读取时索引整体错位。
//
// AddKey(key)：
//   把 key 追加进 keys_，把起点记进 start_。
//
// Finish()：
//   1. 若还有未生成的段（start_ 非空），先 GenerateFilter()
//   2. array_offset = result_.size()
//   3. 把 filter_offsets_ 逐个 PutFixed32 进 result_
//   4. PutFixed32(result_, array_offset)  ← 偏移数组的起点
//   5. result_.push_back(kFilterBaseLg)
//   返回 Slice(result_)
//
// 想一想：为什么偏移数组放在位图**后面**而不是前面？
// （提示：写入时你还不知道会有几段，位图长度也只有生成后才知道。
//   把「地址表」放在最后，就不需要预留空间或回填。）
// ---------------------------------------------------------------------------
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  (void)block_offset;
  // 占位实现：不切段。按上面写：算出 filter_index，用 while（不是 if）
  // 把中间跨过的段逐个 GenerateFilter() 补成空 filter。
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  (void)key;
  // 占位实现：一个 key 都不记。按上面写：start_ 记起点，keys_ 追加内容。
}

Slice FilterBlockBuilder::Finish() {
  // 占位实现：返回空内容。FilterBlockReader 见到长度 < 5 会判「没有 filter」，
  // 于是所有 KeyMayMatch 返回 true——功能上不丢数据，但读放大翻好几倍，
  // DbRead.FilterKeepsMissesCheap 会红。
  // 按上面 5 步写：注意偏移数组在**位图后面**。
  return Slice();
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // 这一段没有任何 key（块之间有空隙，或数据太小够不到这一段）。
    // 仍然要 push 一个 offset：段数是「位置」决定的，不是「有没有 key」。
    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    return;
  }

  // 追加哨兵，让最后一对 (start_[i], start_[i+1]) 也能算出长度
  start_.push_back(static_cast<uint32_t>(keys_.size()));

  std::vector<Slice> keys(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    keys[i] = Slice(keys_.data() + start_[i], start_[i + 1] - start_[i]);
  }

  filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
  policy_->CreateFilter(keys.data(), static_cast<int>(num_keys), &result_);

  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy, const Slice& contents)
    : policy_(policy), contents_(contents.ToString()) {
  const size_t n = contents_.size();
  if (n < 5) return;  // 4 字节偏移起点 + 1 字节 base_lg 是最小长度

  base_lg_ = static_cast<size_t>(static_cast<unsigned char>(contents_[n - 1]));
  if (base_lg_ > 63) return;  // 移位超过 63 是未定义行为

  const uint32_t last_word = DecodeFixed32(contents_.data() + n - 5);
  if (static_cast<size_t>(last_word) > n - 5) return;

  offsets_ = contents_.data() + last_word;
  data_limit_ = last_word;
  num_ = (n - 5 - last_word) / 4;
  valid_ = true;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) const {
  if (!valid_) {
    // 没有 filter（老文件或未配置）。返回 true = 「我不知道，你去读块」。
    return true;
  }
  const uint64_t index = block_offset >> base_lg_;
  if (index >= num_) {
    // 偏移超出已记录的段数。理论上不该发生；保守返回 true。
    return true;
  }

  const uint32_t start = DecodeFixed32(offsets_ + index * 4);
  const uint32_t limit = (index + 1 < num_)
                             ? DecodeFixed32(offsets_ + (index + 1) * 4)
                             : static_cast<uint32_t>(data_limit_);
  if (start > limit || limit > contents_.size()) return true;  // 损坏
  if (start == limit) return false;  // 空 filter：这一段确实没有 key

  return policy_->KeyMayMatch(key, Slice(contents_.data() + start, limit - start));
}

}  // namespace lsm
