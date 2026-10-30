// Bloom filter 的两条硬性质：绝不能有假阴性；假阳性率要在理论上界内。
#include <memory>
#include <string>
#include <vector>

#include "lsm/filter_policy.h"
#include "test_macros.h"
#include "key_util.h"

using namespace lsm;

namespace {

std::string BuildFilter(const FilterPolicy* policy, const std::vector<std::string>& keys) {
  std::vector<Slice> slices;
  slices.reserve(keys.size());
  for (const std::string& k : keys) slices.push_back(Slice(k));
  std::string dst;
  policy->CreateFilter(slices.data(), static_cast<int>(slices.size()), &dst);
  return dst;
}

}  // namespace

TEST(Bloom, NoFalseNegative) {
  std::unique_ptr<const FilterPolicy> policy = NewBloomFilterPolicy(10);
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back(lsm_test::Key(static_cast<uint64_t>(i)));

  const std::string filter = BuildFilter(policy.get(), keys);
  ASSERT_TRUE(filter.size() > 2);

  // 每一个插进去的 key 都必须说「可能在」。
  // 有一条失败就意味着会丢数据 —— 这是 Bloom filter 唯一不可妥协的性质。
  int missing = 0;
  for (const std::string& k : keys) {
    if (!policy->KeyMayMatch(Slice(k), Slice(filter))) ++missing;
  }
  EXPECT_EQ(missing, 0);
}

TEST(Bloom, FprWithinBound) {
  const int bits_per_key = 10;
  std::unique_ptr<const FilterPolicy> policy = NewBloomFilterPolicy(bits_per_key);

  std::vector<std::string> inserted;
  for (int i = 0; i < 10000; ++i) {
    inserted.push_back(lsm_test::Key(static_cast<uint64_t>(i)));
  }
  const std::string filter = BuildFilter(policy.get(), inserted);

  // 查 10000 个不存在的 key，统计被误判为「可能在」的比例。
  // bits_per_key = 10 的理论假阳性率约 1%，这里给到 2% 的容差。
  int false_positive = 0;
  const int probes = 10000;
  for (int i = 0; i < probes; ++i) {
    const std::string k = "absent-" + lsm_test::Padded(static_cast<uint64_t>(i));
    if (policy->KeyMayMatch(Slice(k), Slice(filter))) ++false_positive;
  }
  const double fpr = static_cast<double>(false_positive) / probes;
  EXPECT_TRUE(fpr <= 0.02);
}

TEST(Bloom, HigherBitsPerKeyLowersFpr) {
  // 不是必需的性质，但能抓出「bits_per_key 根本没被用上」这类 bug。
  std::vector<std::string> inserted;
  for (int i = 0; i < 5000; ++i) {
    inserted.push_back(lsm_test::Key(static_cast<uint64_t>(i)));
  }

  std::unique_ptr<const FilterPolicy> low = NewBloomFilterPolicy(4);
  std::unique_ptr<const FilterPolicy> high = NewBloomFilterPolicy(16);
  const std::string low_filter = BuildFilter(low.get(), inserted);
  const std::string high_filter = BuildFilter(high.get(), inserted);

  // 16 bits/key 的位图应该明显更大
  EXPECT_TRUE(high_filter.size() > low_filter.size());

  int low_fp = 0;
  int high_fp = 0;
  for (int i = 0; i < 5000; ++i) {
    const std::string k = "absent-" + lsm_test::Padded(static_cast<uint64_t>(i));
    if (low->KeyMayMatch(Slice(k), Slice(low_filter))) ++low_fp;
    if (high->KeyMayMatch(Slice(k), Slice(high_filter))) ++high_fp;
  }
  EXPECT_TRUE(high_fp <= low_fp);
}

TEST(Bloom, EmptyFilterSaysNo) {
  std::unique_ptr<const FilterPolicy> policy = NewBloomFilterPolicy(10);
  // 长度不足 2 的 filter 表示「什么都没插」，必须返回 false
  EXPECT_FALSE(policy->KeyMayMatch(Slice("anything"), Slice("")));
  EXPECT_FALSE(policy->KeyMayMatch(Slice("anything"), Slice(std::string(1, '\0'))));
}

TEST(Bloom, CorruptKReturnsTrue) {
  std::unique_ptr<const FilterPolicy> policy = NewBloomFilterPolicy(10);
  // 最后一个字节是 k。设成 200（> 30）表示元数据损坏：
  // 必须返回 true（宁可多读一次盘），返回 false 会丢数据。
  std::string filter(100, '\x7f');
  filter.push_back(static_cast<char>(200));
  EXPECT_TRUE(policy->KeyMayMatch(Slice("whatever"), Slice(filter)));
}

TEST(Bloom, SmallKeySetStillGetsMinimumBits) {
  // n 很小时 bits 会被抬到 64 的下限，否则一个只含 1 个 key 的块
  // 会得到 1 字节位图 + k 字节，假阳性高得离谱。
  std::unique_ptr<const FilterPolicy> policy = NewBloomFilterPolicy(10);
  std::vector<std::string> one = {"only"};
  const std::string filter = BuildFilter(policy.get(), one);
  EXPECT_TRUE(filter.size() >= 9);  // 8 字节位图 + 1 字节 k
  EXPECT_TRUE(policy->KeyMayMatch(Slice("only"), Slice(filter)));
}
