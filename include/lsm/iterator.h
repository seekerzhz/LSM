// Iterator：有序键值流的抽象。
//
// 所有具体实现（MemTable、Block、MergingIterator、CompactionIterator）
// 都实现这个接口，于是上层代码不需要知道自己面对的是内存表还是磁盘文件。
#pragma once

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// 一次 Get 的统计信息，用于诊断读放大
struct GetStats {
  int blocks_read = 0;        // 实际读了多少个 data block
  int filter_checks = 0;      // 检查了多少个 Bloom filter
  bool filter_rejected = false;  // 是否被 filter 排除（本次零读盘）
};

class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;              // 反向扫描用；实现可以很慢
  virtual void Seek(const Slice& target) = 0; // 定位到第一个 >= target
  virtual void Next() = 0;
  virtual void Prev() = 0;                    // 实现可以很慢

  virtual Slice key() const = 0;              // internal key
  virtual Slice value() const = 0;
  virtual Status status() const = 0;
};

// 永远无效、且 status() 返回 s 的迭代器。
// 用于「打开资源失败但调用方期望拿到迭代器」的场合：返回 nullptr 会让
// 调用方在别处解引用崩掉，错误信息也丢了。
Iterator* NewErrorIterator(const Status& s);

}  // namespace lsm
