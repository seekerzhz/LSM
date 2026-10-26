#include "lsm/skiplist.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lsm/arena.h"
#include "tests/test_macros.h"

namespace lsm {
namespace {

// 按字典序比较两个 C 字符串
struct CStrComparator {
  int operator()(const char* a, const char* b) const {
    return std::strcmp(a, b);
  }
};

// 用一组固定的 key，保证指针生命周期覆盖整个测试
const char* const kKeys[] = {"alpha", "beta", "delta", "epsilon", "gamma",
                             "kappa", "lambda", "omega", "sigma", "zeta"};

TEST(SkipList, InsertContains) {
  Arena arena;
  SkipList<const char*, CStrComparator> list(CStrComparator(), &arena);

  for (const char* k : kKeys) list.Insert(k);
  for (const char* k : kKeys) EXPECT_TRUE(list.Contains(k));
  EXPECT_FALSE(list.Contains("not-there"));
  EXPECT_FALSE(list.Contains("aaa"));  // 比所有元素都小
  EXPECT_FALSE(list.Contains("zzz"));  // 比所有元素都大
}

TEST(SkipList, IteratorSeek) {
  Arena arena;
  SkipList<const char*, CStrComparator> list(CStrComparator(), &arena);
  for (const char* k : kKeys) list.Insert(k);

  SkipList<const char*, CStrComparator>::Iterator it(&list);

  it.Seek("delta");
  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(std::strcmp(it.key(), "delta") == 0);

  // Seek 到不存在的 key 应停在第一个更大的位置
  it.Seek("epsilon1");
  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(std::strcmp(it.key(), "gamma") == 0);

  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(std::strcmp(it.key(), "alpha") == 0);

  it.SeekToLast();
  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(std::strcmp(it.key(), "zeta") == 0);

  // 遍历一遍，必须严格递增
  it.SeekToFirst();
  std::string prev;
  int count = 0;
  while (it.Valid()) {
    const std::string cur(it.key());
    if (!prev.empty()) EXPECT_TRUE(prev < cur);
    prev = cur;
    ++count;
    it.Next();
  }
  EXPECT_EQ(static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0])), count);
}

TEST(SkipList, InsertManySorted) {
  Arena arena;
  SkipList<const char*, CStrComparator> list(CStrComparator(), &arena);

  // 用静态池保证字符串指针稳定
  static std::vector<std::string> pool;
  pool.clear();
  pool.reserve(500);
  for (int i = 0; i < 500; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", (i * 37) % 500);  // 乱序插入
    pool.emplace_back(buf);
  }
  for (const auto& s : pool) list.Insert(s.c_str());

  int seen = 0;
  SkipList<const char*, CStrComparator>::Iterator it(&list);
  it.SeekToFirst();
  std::string prev;
  while (it.Valid()) {
    const std::string cur(it.key());
    if (!prev.empty()) EXPECT_TRUE(prev < cur);
    prev = cur;
    ++seen;
    it.Next();
  }
  EXPECT_EQ(500, seen);
}

}  // namespace
}  // namespace lsm
