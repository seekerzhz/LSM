// TableBuilder：把一个有序的 internal key 流写成一个合法的 SST 文件。
//
// 写入是**严格的顺序追加**，一个字节都不能回退：所以元数据（filter /
// metaindex / index / footer）只能放在文件尾部。读取则相反，先读尾部拿到
// 所有入口，再跳着读。这个不对称是所有接口设计的起点。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/block_builder.h"
#include "lsm/coding.h"
#include "lsm/db.h"  // Options
#include "lsm/env.h"
#include "lsm/filter_block.h"
#include "lsm/filter_policy.h"
#include "lsm/format.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// metaindex 里 filter 块的键名。拼错不会报错，只是查不到 filter——
// 症状是读放大从 1.0 变成 5.0。
inline const char* const kFilterMetaKey = "filter.bloom";

class TableBuilder {
 public:
  TableBuilder(const Options& options, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 追加一条。key 必须严格递增（internal key 的序），否则 status() 变 Invalid。
  void Add(const Slice& key, const Slice& value);

  // 收尾：写最后一个 data block、filter、metaindex、index、footer。
  // 成功返回 OK，此时文件内容已经完整（但未 fsync）。
  Status Finish();

  Status status() const { return status_; }

  // 放弃：不写尾部，直接置为关闭。用于 compaction 中途发现要停。
  void Abandon();

  uint64_t FileSize() const { return offset_; }
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t NumDataBlocks() const { return num_data_blocks_; }

 private:
  bool ok() const { return status_.ok() && !closed_; }

  // 把当前 data_block_ 落盘，并把「末 key -> handle」加进索引
  void Flush();
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(const Slice& data, BlockHandle* handle);

  Options options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;

  // 声明顺序 = 初始化顺序：icmp_ 必须在这两个 BlockBuilder 之前，
  // 因为它们持的是指向 icmp_ 的指针。
  InternalKeyComparator icmp_;
  BlockBuilder data_block_{&icmp_};
  BlockBuilder index_block_{&icmp_};
  std::string last_key_;
  uint64_t num_entries_ = 0;
  uint64_t num_data_blocks_ = 0;
  bool closed_ = false;

  std::unique_ptr<const FilterPolicy> policy_;
  std::unique_ptr<FilterBlockBuilder> filter_block_;
};

}  // namespace lsm
