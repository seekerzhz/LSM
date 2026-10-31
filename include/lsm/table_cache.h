// TableCache：已打开的 SST 句柄池。
//
// 没有它，每次 Get 都要 open + 读 footer + 读 index —— 一个「查 1 个 key」
// 的操作变成三次系统调用。有了它，文件只在第一次访问时打开。
//
// 本课程不做 LRU 淘汰：SST 数量在 compaction 下稳定在几十个，
// 全部常驻的开销远小于维护一个淘汰链表的复杂度。真实系统（LevelDB）
// 是有限容量的 LRU，因为它们的文件数可能上千。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lsm/db.h"
#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"
#include "lsm/table.h"

namespace lsm {

class TableCache {
 public:
  TableCache(const Options& options, const std::string& dbname);
  ~TableCache();

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // 找到或打开编号为 file_number 的表。
  // 返回的 Table* 由 cache 拥有：调用方不得 delete，也不得在 Evict 后继续用。
  Status FindTable(uint64_t file_number, uint64_t file_size, Table** table);

  // 在单个文件内查 internal key（语义见 Table::InternalGet）
  Status Get(const ReadOptions& opt, uint64_t file_number, uint64_t file_size,
             const Slice& internal_key, std::string* value, ValueType* type,
             GetStats* stats);

  // 顺序迭代整个文件。返回的迭代器**持有 Table 的引用**，
  // 所以即使中途被 Evict，正在扫描的迭代器也不会崩。
  Iterator* NewIterator(uint64_t file_number, uint64_t file_size);

  // 删文件前必须先踢掉缓存项，否则 fd 还开着
  void Evict(uint64_t file_number);
  void Clear();

  size_t size() const;

  static std::string FileName(const std::string& dbname, uint64_t number);

 private:
  Options options_;
  std::string dbname_;
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, std::shared_ptr<Table>> cache_;
};

}  // namespace lsm
