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
  if (!ok()) return;

  assert(!closed_);
  // 有序性检查必须用比较器：internal key 的尾部是 seq，字节序不单调
  if (num_entries_ > 0 && icmp_(key, Slice(last_key_)) <= 0) {
    // 有序性被破坏。继续写下去，索引里的「末 key」就不再是块内最大值，
    // 二分查找会定位到错误的块——而且是静默的错。
    status_ = Status::Invalid("table keys must be strictly increasing");
    return;
  }

  data_block_.Add(key, value);
  // filter 里存的是 **user key**，不是 internal key：查 Bloom 时只按用户键查，
  // 否则同一个 key 的每个版本都会算成一个不同的 key，位图被撑爆、
  // 而且 Get 用哪个 seq 去查也对不上。
  filter_block_->AddKey(ExtractUserKey(key));
  last_key_.assign(key.data(), key.size());
  ++num_entries_;

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    Flush();
  }
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
  if (!ok()) return status_;

  Flush();

  BlockHandle filter_block_handle;
  BlockHandle metaindex_block_handle;
  BlockHandle index_block_handle;

  if (ok()) {
    // filter block 自己带长度信息（末尾有偏移数组起点 + base_lg），
    // 所以直接整块写成 raw，不需要再套一层 BlockBuilder
    Slice filter_contents = filter_block_->Finish();
    WriteRawBlock(filter_contents, &filter_block_handle);

    // metaindex：目前只有一条。以后加「压缩字典」「统计信息」都往这里挂，
    // 老读者不认识新 key 就跳过——这就是 metaindex 存在的意义。
    BlockBuilder meta_index_block;
    if (ok()) {
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(Slice(kFilterMetaKey), handle_encoding);
      WriteBlock(&meta_index_block, &metaindex_block_handle);
    }
  }

  if (ok()) {
    WriteBlock(&index_block_, &index_block_handle);
  }

  if (ok()) {
    Footer footer;
    footer.metaindex_handle = metaindex_block_handle;
    footer.index_handle = index_block_handle;
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    status_ = file_->Append(footer_encoding);
    if (status_.ok()) offset_ += footer_encoding.size();
  }

  if (ok()) {
    status_ = file_->Flush();
  }

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
