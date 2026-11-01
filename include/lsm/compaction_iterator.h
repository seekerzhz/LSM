// CompactionIterator：把「归并流」变成「应该写进新文件的流」。
//
// 归并流里同一个 user_key 会出现多次（从新到旧排列）。这个包装器负责：
//   1. 只保留每个 user_key 的**最新**一条（旧的被覆盖，没有意义）
//   2. 最底层的墓碑连自己一起丢掉（下面没有更旧版本了，墓碑的使命结束）
//
// 把丢弃逻辑独立成迭代器，好处是它可以单独测：构造两个假迭代器，
// 验证输出流的形状，完全不需要碰磁盘。
#pragma once

#include <string>

#include "lsm/compaction.h"
#include "lsm/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

class CompactionIterator : public Iterator {
 public:
  // input：MergingIterator（归并好的 internal key 流）
  // compaction：用来判断「墓碑能不能丢」
  CompactionIterator(Iterator* input, const InternalKeyComparator* icmp, Compaction* compaction);

  bool Valid() const override { return valid_; }
  void SeekToFirst() override;
  void Seek(const Slice& target) override;
  void Next() override;

  Slice key() const override;
  Slice value() const override;
  Status status() const override;

  // 丢弃逻辑只对正向流有意义
  void SeekToLast() override;
  void Prev() override;

  // 统计：这轮 compaction 丢了多少条（测试与报告用）
  uint64_t num_dropped() const { return num_dropped_; }

 private:
  // 从当前位置开始，跳过所有该丢的记录，停在第一条要输出的记录上
  void FindNextUserEntry();
  void SkipToEnd();

  Iterator* input_;
  const InternalKeyComparator* icmp_;
  Compaction* compaction_;
  bool valid_ = false;
  Status status_;
  std::string current_user_key_;
  bool have_current_user_key_ = false;
  uint64_t num_dropped_ = 0;
};

}  // namespace lsm
