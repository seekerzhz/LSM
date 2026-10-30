#include "lsm/table_builder.h"

#include <cassert>

namespace lsm {

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options),
      file_(file),
      policy_(NewBloomFilterPolicy(options.bits_per_key)),
      filter_block_(new FilterBlockBuilder(policy_.get())) {}

TableBuilder::~TableBuilder() {
  // 没 Finish 也没 Abandon 就析构：文件里是一段没有 footer 的垃圾。
  // 调用方（recorder / compaction）负责删掉 .tmp 文件。
  assert(closed_);
}

// ---------------------------------------------------------------------------
// TODO(2.4) TableBuilder::Add
//
// 约束：
//   1. key <= last_key_ 时把 status_ 置为 Invalid 并返回（不能静默写坏）
//   2. data_block_.Add(key, value)
//   3. last_key_ = key，++num_entries_
//   4. 若 data_block_.CurrentSizeEstimate() >= options_.block_size，Flush()
//
// 想一想：为什么阈值判断放在 Add 之后而不是之前？
// （提示：块的大小只有加进这一条之后才知道；放在之前会导致块刚好差一条就超。）
// ---------------------------------------------------------------------------
void TableBuilder::Add(const Slice& key, const Slice& value) {
  (void)key;
  (void)value;
  // 占位实现：一条都不写。Table.* 会全部失败（表是空的）。
  // 按上面 4 步填；第 3 步 last_key_ 忘了的话，Flush 时索引项的末 key
  // 就是上一块的值，二分查找会把你送到相邻块。
}

// ---------------------------------------------------------------------------
// TODO(2.5) TableBuilder::Finish
//
// 顺序（写错顺序文件就不合法）：
//   1. Flush() —— 落最后一个 data block
//   2. filter_block_->Finish() -> WriteRawBlock -> 得到 filter_handle
//   3. metaindex block：只有一条 "filter.bloom" -> filter_handle
//   4. index block
//   5. 48 字节 footer（两个 handle + magic）
//   6. file_->Flush()
//
// 注意：**不在这里 Sync()**。一次 flush / compaction 可能写多个文件，
// 由调用方统一 sync 更高效，而且「什么时候数据算落了盘」这个决定应该
// 留给上层（它才知道事务边界在哪）。
//
// 想一想：如果 metaindex 里 filter 的 key 拼错了，会发生什么？
// （答案：不报错。Table 读不到 filter，KeyMayMatch 走「没有 filter」分支
//   返回 true，一切照常工作，只是每次 Get 都真的去读块。读放大翻 5 倍。）
// ---------------------------------------------------------------------------
Status TableBuilder::Finish() {
  // 占位实现：不写任何块、不写 footer，只把 closed_ 置上（否则析构里的
  // assert(closed_) 会炸，让学生误以为是析构函数写错了）。
  // 症状：文件里没有魔法数，Table::Open 返回 Corruption("bad magic")。
  closed_ = true;
  return status_;
}

void TableBuilder::Abandon() {
  closed_ = true;
}

void TableBuilder::Flush() {
  if (!ok()) return;
  if (data_block_.empty()) return;

  BlockHandle handle;
  WriteBlock(&data_block_, &handle);
  if (!ok()) return;

  // 索引项存的是「这个块的最后一个 key」。为什么不是第一个：
  // 查找时用 index_iter->Seek(target) 找第一个 key >= target 的索引项，
  // 存末 key 才能保证「被选中的块一定包含 target 或它的后继」。
  // 存首 key 的话，target 落在块尾部时会漏到下一块。
  std::string handle_encoding;
  handle.EncodeTo(&handle_encoding);
  index_block_.Add(Slice(last_key_), handle_encoding);

  ++num_data_blocks_;

  // 告诉 filter：下一个 data block 从 offset_ 这个位置开始
  filter_block_->StartBlock(offset_);
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  Slice raw = block->Finish();
  WriteRawBlock(raw, handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& data, BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(data.size());
  status_ = file_->Append(data);
  if (status_.ok()) {
    offset_ += data.size();
  }
}

}  // namespace lsm
