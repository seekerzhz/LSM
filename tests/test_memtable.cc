#include "lsm/memtable.h"

#include <string>

#include "lsm/dbformat.h"
#include "tests/test_macros.h"

namespace lsm {
namespace {

// MemTable 的析构是 private 语义（靠 Unref），所以测试里用裸 new + Unref
class Fixture {
 public:
  Fixture() : table_(new MemTable(InternalKeyComparator())) { table_->Ref(); }
  ~Fixture() { table_->Unref(); }
  MemTable* get() { return table_; }

 private:
  MemTable* table_;
};

TEST(MemTable, SameKeyKeepsAllVersions) {
  Fixture f;
  f.get()->Add(1, kTypeValue, Slice("k"), Slice("v1"));
  f.get()->Add(2, kTypeValue, Slice("k"), Slice("v2"));
  f.get()->Add(3, kTypeValue, Slice("k"), Slice("v3"));

  // 三个版本都在表里：用 seq=3 读，读到 v3；用 seq=2 读，读到 v2
  {
    LookupKey lk("k", 3);
    std::string v;
    Status s;
    ASSERT_TRUE(f.get()->Get(lk, &v, &s));
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(v == "v3");
  }
  {
    LookupKey lk("k", 2);
    std::string v;
    Status s;
    ASSERT_TRUE(f.get()->Get(lk, &v, &s));
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(v == "v2");
  }
}

TEST(MemTable, GetMissReturnsFalse) {
  Fixture f;
  f.get()->Add(1, kTypeValue, Slice("abc"), Slice("1"));

  // 比 abc 大一点但不存在，用于抓「没校验 user_key 相等」的 bug
  {
    LookupKey lk("abd", 2);
    std::string v;
    Status s;
    EXPECT_FALSE(f.get()->Get(lk, &v, &s));
  }
  // 比 abc 小
  {
    LookupKey lk("abb", 2);
    std::string v;
    Status s;
    EXPECT_FALSE(f.get()->Get(lk, &v, &s));
  }
}

TEST(MemTable, TombstoneReportsNotFound) {
  Fixture f;
  f.get()->Add(1, kTypeValue, Slice("k"), Slice("v"));
  f.get()->Add(2, kTypeDeletion, Slice("k"), Slice());

  LookupKey lk("k", 3);
  std::string v;
  Status s;
  const bool found = f.get()->Get(lk, &v, &s);
  ASSERT_TRUE(found);          // 找到了墓碑
  EXPECT_TRUE(s.IsNotFound()); // 语义是"不存在"
}

TEST(MemTable, MemoryUsageGrows) {
  Fixture f;
  const size_t before = f.get()->ApproximateMemoryUsage();
  for (int i = 0; i < 1000; ++i) {
    const std::string k = "key" + std::to_string(i);
    f.get()->Add(static_cast<SequenceNumber>(i + 1), kTypeValue, Slice(k), Slice("value"));
  }
  const size_t after = f.get()->ApproximateMemoryUsage();
  EXPECT_TRUE(after > before);
  // 每条至少占 16 字节的 MemEntry
  EXPECT_TRUE(after - before >= 1000 * 16);
}

TEST(MemTable, IteratorOrdered) {
  Fixture f;
  const char* keys[] = {"c", "a", "d", "b"};
  for (int i = 0; i < 4; ++i) {
    f.get()->Add(static_cast<SequenceNumber>(i + 1), kTypeValue, Slice(keys[i]),
                 Slice("v"));
  }
  Iterator* it = f.get()->NewIterator();
  it->SeekToFirst();
  std::string prev;
  int count = 0;
  while (it->Valid()) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(it->key(), &p));
    const std::string cur = p.user_key.ToString();
    if (!prev.empty()) EXPECT_TRUE(prev < cur);
    prev = cur;
    ++count;
    it->Next();
  }
  EXPECT_EQ(4, count);
  delete it;
}

}  // namespace
}  // namespace lsm
