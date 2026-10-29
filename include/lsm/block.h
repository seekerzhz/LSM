// Block：一个已经读进内存的块，负责「按 internal key 定位条目」。
//
// 不拥有内存的迭代器会在块被释放后变成悬垂指针，所以这个类的设计前提是：
// 调用方（Table / TableBuilder）保证 Block 活得比它的迭代器久。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "lsm/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

class Block {
 public:
  // 接管整块字节（含块尾的重启点数组）。构造时只解析「重启点在哪」，
  // 不逐条解析 entry —— 读进来的块往往只用得到一条记录。
  explicit Block(std::string contents);

  size_t size() const { return size_; }
  size_t NumRestarts() const { return num_restarts_; }

  // 迭代器里的 Slice 全部指向本 Block 的内部缓冲区。
  // comparator 为空时按字节序比较（metaindex 这类非 internal key 的块用它）。
  Iterator* NewIterator(const InternalKeyComparator* comparator) const;

  // 顺序扫一遍数条目数。只用于 dump 工具，不要在热路径上调用。
  size_t CountEntries() const;

  bool IsCorrupted() const { return status_.IsCorruption(); }
  const Status& status() const { return status_; }

  class Iter;

 private:
  friend class Iter;

  std::string data_;
  size_t size_ = 0;
  size_t restarts_offset_ = 0;  // entries 的结束位置，也是重启点数组的起点
  uint32_t num_restarts_ = 0;
  Status status_;
};

}  // namespace lsm
