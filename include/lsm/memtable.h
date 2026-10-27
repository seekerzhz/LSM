// MemTable：内存里的有序键值表，底层是 SkipList。
//
// 存储布局（每条 entry 一次连续分配）：
//   [key_len:uint32][tag:uint64][value_len:uint32][key bytes][value bytes]
//   tag = (sequence << 8) | type
//
// 为什么用「一次分配 + 直接 memcpy」而不是 std::string：省一次堆分配 + 省
// 一个 std::string 的头部；而且整个表在 Arena 里，析构时不用逐个释放。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "lsm/arena.h"
#include "lsm/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/skiplist.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// 一条内存记录的物理布局。课程提供（完整）。
// 它只是"字节长什么样"，不是 MemTable 的业务接口，因此没有封装。
struct MemEntry {
  uint32_t key_len;
  uint64_t tag;        // (seq << 8) | type
  uint32_t value_len;

  const char* key_data() const {
    return reinterpret_cast<const char*>(this) + sizeof(MemEntry);
  }
  const char* value_data() const { return key_data() + key_len; }
  // 这里存的是 **internal key**（user_key + 8 字节尾部），长度含尾部
  Slice key() const { return Slice(key_data(), key_len); }
  Slice value() const { return Slice(value_data(), value_len); }
};

class MemTable {
 public:
  explicit MemTable(const InternalKeyComparator& cmp);
  ~MemTable() = default;

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    const int prev = refs_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
  }
  int refs() const { return refs_.load(std::memory_order_relaxed); }

  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // 查询。
  //  返回 false                       -> 这个 key 在表里不存在（继续查下一层）
  //  返回 true 且 s->ok()             -> 找到，*value 是内容
  //  返回 true 且 s->IsNotFound()     -> 找到墓碑（删除标记）
  //  返回 true 且 s 是 Corruption     -> 表内数据损坏
  bool Get(const LookupKey& key, std::string* value, Status* s);

  Iterator* NewIterator();
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  // 供 Iterator 使用：从一条 entry 的指针取出 Slice
  static Slice EntryKey(const char* entry) {
    return reinterpret_cast<const MemEntry*>(entry)->key();
  }
  static Slice EntryValue(const char* entry) {
    return reinterpret_cast<const MemEntry*>(entry)->value();
  }

 private:
  // SkipList 需要一个可调用对象，而 internal key 的比较依赖 comparator 实例，
  // 所以把 InternalKeyComparator 包进这个结构体，作为 SkipList 的模板参数。
  // 声明为 public 是因为 MemTableIterator（在 .cc 里）需要用到它的类型。
 public:
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  // 声明顺序 = 初始化顺序：comparator_ 必须在 table_ 之前，arena_ 也是。
 private:
  KeyComparator comparator_;
  Arena arena_;
  SkipList<const char*, KeyComparator> table_;
  std::atomic<int> refs_;
};

}  // namespace lsm
