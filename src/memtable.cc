#include "lsm/memtable.h"

#include <cstring>

#include "lsm/coding.h"

namespace lsm {

// ---------------------------------------------------------------------------
// 课程提供：比较器适配（把 char* 转成 Slice 再交给 InternalKeyComparator）
// ---------------------------------------------------------------------------
int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator(EntryKey(a), EntryKey(b));
}

namespace {

// 构造一个"伪 entry"：把一个裸 internal key 包成 MemEntry 布局，
// 这样 SkipList 的比较器（它把 key 当成 MemEntry*）才能正确比较。
// 课程提供（完整）—— 这是容易踩的坑，不需要学生自己发明。
const char* EncodeTempKey(std::string* scratch, const Slice& target) {
  MemEntry e{};
  e.key_len = static_cast<uint32_t>(target.size());
  e.tag = 0;
  e.value_len = 0;
  scratch->assign(reinterpret_cast<const char*>(&e), sizeof(MemEntry));
  scratch->append(target.data(), target.size());
  return scratch->data();
}

// 把 SkipList 的迭代器包装成 Iterator，把 entry 解成 (internal key, value)
class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(SkipList<const char*, MemTable::KeyComparator>::Iterator iter)
      : iter_(iter) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Seek(const Slice& target) override {
    tmp_.clear();
    iter_.Seek(EncodeTempKey(&tmp_, target));
  }
  void Next() override { iter_.Next(); }
  void Prev() override {
    // SkipList 不支持反向，这里退化为「从表头扫到当前位置的前一个」。
    // 只在测试反向扫描时用，性能不是重点。
    if (!iter_.Valid()) return;
    const std::string cur = MemTable::EntryKey(iter_.key()).ToString();
    iter_.SeekToFirst();
    std::string prev;
    while (iter_.Valid()) {
      const std::string k = MemTable::EntryKey(iter_.key()).ToString();
      if (k >= cur) break;
      prev = k;
      iter_.Next();
    }
    if (prev.empty()) {
      iter_.SeekToFirst();
    } else {
      tmp_.clear();
      iter_.Seek(EncodeTempKey(&tmp_, Slice(prev)));
    }
  }

  Slice key() const override { return MemTable::EntryKey(iter_.key()); }
  Slice value() const override { return MemTable::EntryValue(iter_.key()); }
  Status status() const override { return Status::OK(); }

 private:
  SkipList<const char*, MemTable::KeyComparator>::Iterator iter_;
  std::string tmp_;
};

}  // namespace

MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp), arena_(), table_(comparator_, &arena_), refs_(0) {}

// ---------------------------------------------------------------------------
// TODO(1.6) MemTable::Add
//
// 输入：seq, type, key（user key）, value
// 输出：无（把一条记录插进 table_）
// 约束：
//   1. 构造 internal key：把 key 和 8 字节尾部 (seq<<8|type) 连起来，
//      不要依赖调用者已经拼好。可以先用 std::string 拼接（简单），
//      也可以直接算出总长度再 memcpy（快）。
//   2. 一次 AllocateAligned 分配 sizeof(MemEntry) + internal_key_len + value_len
//   3. 填 key_len / tag / value_len，然后 memcpy key、value
//   4. table_.Insert(entry_ptr)
//   注意：tag 字段在这里存 (seq<<8|type)，而 key 里已经含尾部——
//         两者都要写，前者方便快速取 seq，后者用于排序。
// ---------------------------------------------------------------------------
void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  (void)seq;
  (void)type;
  (void)key;
  (void)value;
  // 占位实现：什么都不做，测试会失败
}

// ---------------------------------------------------------------------------
// TODO(1.7) MemTable::Get
//
// 输入：LookupKey（内部已拼好 internal key）
// 输出：见头文件注释的四种返回语义
// 约束：
//   1. 必须 Seek 到 lookup key 的位置，而不是从表头线性扫
//   2. 定位后必须校验 user_key 相等（否则会返回相邻 key 的值）
//   3. 命中墓碑时返回 true 且 *s = Status::NotFound(...)
//   4. ParseInternalKey 失败时 *s = Status::Corruption(...)
// ---------------------------------------------------------------------------
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  (void)key;
  (void)value;
  *s = Status::OK();
  // 占位实现：永远说"没找到"，上层会继续查下一层
  return false;
}

Iterator* MemTable::NewIterator() {
  return new MemTableIterator(SkipList<const char*, KeyComparator>::Iterator(&table_));
}

}  // namespace lsm
