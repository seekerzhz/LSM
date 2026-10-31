// Table：一个只读的 SST。
//
// 打开成本很低：读 48 字节 footer + 一个 index block。filter 是懒加载的——
// 一个刚写完就再也不会被读的文件，不该在打开时就把位图读进内存。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "lsm/block.h"
#include "lsm/db.h"  // Options / ReadOptions
#include "lsm/dbformat.h"
#include "lsm/env.h"
#include "lsm/filter_block.h"
#include "lsm/filter_policy.h"
#include "lsm/format.h"
#include "lsm/iterator.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

class Table {
 public:
  // file 的所有权转移给 Table（SST 打开后文件句柄跟着 Table 活）。
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                     uint64_t file_size, std::unique_ptr<Table>* table);

  // 全表顺序迭代。compaction 用它把输入文件读成一条流。
  Iterator* NewIterator() const;

  // -------------------------------------------------------------------------
  // 在单个文件内查 internal_key。
  //
  // 返回 OK        -> 命中一条记录，*type 说明它是值还是墓碑。
  //                    **墓碑也返回 OK**：调用方必须据此停止往更旧的文件查，
  //                    否则删除会「复活」。
  // 返回 NotFound  -> 这个文件里没有（可以换下一个文件继续查）
  // -------------------------------------------------------------------------
  Status InternalGet(const ReadOptions& opt, const Slice& key, std::string* value,
                     ValueType* type, GetStats* stats);

  uint64_t file_size() const { return file_size_; }
  size_t NumDataBlocks() const;
  const BlockHandle& filter_handle() const { return filter_handle_; }
  bool has_filter() const { return filter_handle_.size() != kInvalidHandle; }

  // 调试用：按索引顺序逐个访问 data block。
  // sst_dump 用它做「按块分组打印」，也让测试能直接检查块边界。
  void ForEachBlock(int start_index, const std::function<void(int, const Block&)>& fn) const;

  // 懒加载；没有 filter 时返回 nullptr（调用方当作「都可能命中」）
  const FilterBlockReader* filter() const;

 private:
  static constexpr uint64_t kInvalidHandle = ~static_cast<uint64_t>(0);

  friend class TableIterator;

  Table(const Options& options, std::unique_ptr<RandomAccessFile> file, uint64_t file_size);

  Status ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* block) const;
  void ReadFilter() const;  // 失败时静默：没有 filter 只是变慢，不是错误

  Options options_;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_size_ = 0;
  std::unique_ptr<Block> index_block_;
  BlockHandle filter_handle_;
  std::unique_ptr<const FilterPolicy> policy_;
  InternalKeyComparator icmp_;

  // filter 是懒加载的，而 InternalGet 是 const 且可能被多个线程同时调用
  mutable std::mutex filter_mu_;
  mutable bool filter_loaded_ = false;
  mutable std::unique_ptr<FilterBlockReader> filter_reader_;
};

}  // namespace lsm
