// MergingIterator：k 路归并。
//
// 输入：多个按 internal key 升序的迭代器（同一 user key 的多个版本按 seq 降序排）。
// 输出：一个全局有序流。同一个 user key 的多个版本都会出现（去重由
//       上层 CompactionIterator / Get 负责）。
#pragma once

#include <cstdint>
#include <vector>

#include "lsm/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

class MergingIterator : public Iterator {
 public:
  MergingIterator(const InternalKeyComparator* comparator,
                  std::vector<Iterator*>* children);
  ~MergingIterator() override;

  bool Valid() const override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const Slice& target) override;
  void Next() override;
  void Prev() override;
  Slice key() const override;
  Slice value() const override;
  Status status() const override;

 private:
  // ---------------------------------------------------------------------------
  // TODO(2.10) FindSmallest
  // 在 children_ 中选出 internal key 最小的那一路，把下标写进 current_。
  // 用 comparator_ 比较（不要直接 memcmp：虽然本课程的 internal key 编码恰好
  // 让 memcmp 结果正确，但依赖编码细节是脆的）。
  // 没有任何一路 Valid 时把 current_ 置为 -1。
  // 想一想：k 路归并用线性扫 O(k) 还是堆 O(log k)？本章 SST 数量少，线性更快；
  //         第 3 章 compaction 可能归并 20 个文件，那时堆才划算。
  // ---------------------------------------------------------------------------
  void FindSmallest();

  const InternalKeyComparator* comparator_;
  std::vector<Iterator*> children_;
  std::vector<char> valid_;    // 每路当前是否有效（1/0）
  int current_;                // 当前最小者下标，-1 表示无
  bool direction_forward_;
};

}  // namespace lsm
