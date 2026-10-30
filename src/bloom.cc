#include "lsm/filter_policy.h"

#include <cstdint>
#include <cstring>

#include "lsm/coding.h"

namespace lsm {
namespace {

// LevelDB 的 Hash：MurmurHash 的简化变体。
// 不需要密码学强度，只需要「输入差一个字节，输出差很多」，且足够快。
// （两个方法都填上之后这里就不会再报 unused 了。）
[[maybe_unused]] uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  while (n >= 4) {
    uint32_t w = DecodeFixed32(data);
    data += 4;
    n -= 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  // 处理尾部 0~3 字节。顺序不能反（先处理低位再处理高位），
  // 否则 "ab" 和 "ba" 会哈希到同一个值。
  switch (n) {
    case 3:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;
  }
  return h;
}

class BloomFilterPolicy : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // 最优哈希个数 k = ln2 * bits_per_key ≈ 0.69 * b。
    // 夹到 [1, 30]：k 太大时每次查询要算 30 次位运算，
    // 而假阳性率的改善已经可以忽略。
    k_ = static_cast<size_t>(static_cast<double>(bits_per_key) * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  const char* Name() const override { return "lsm.BloomFilter2"; }

  // -------------------------------------------------------------------------
  // TODO(2.8) BloomFilterPolicy::CreateFilter
  //
  // 输入：n 个 key（这些 key 是这个 filter 段覆盖的所有 user_key）
  // 输出：dst 追加 [bitmap][k:1B]
  // 约束：
  //   bits  = max(n * bits_per_key, 64)；bytes = (bits + 7) / 8；bits = bytes*8
  //   dst 先 resize 出 bytes 个 0，最后一个字节写 k
  //   每个 key 算两个哈希 h1 = Hash(key, 0xbc9f1d34)、h2 = Hash(key, 0x5bd1e995)
  //   并把 h2 置为奇数（h2 |= 1），然后第 i 位取 (h1 + i * h2) % bits
  //
  // 为什么 h2 要置奇数：如果 h2 和 bits 有公因子（比如都是偶数），
  // 那么 h1 + i*h2 只能覆盖 bits 的一部分位置，等效位图变小、假阳性升高。
  // 章节里说「测得 12% 而不是 2%」就是这个 bug。
  // -------------------------------------------------------------------------
  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    (void)keys;
    (void)n;
    (void)dst;
    // 占位实现：一个位都不置。生成的 filter 段是空的，
    // 于是 KeyMayMatch 全部返回 false，所有读路径都被拦住。
    // 按上面 4 步填；h2 忘了 `| 1` 的话功能还是对的，只有 FprWithinBound 会挂。
  }

  // -------------------------------------------------------------------------
  // TODO(2.9) BloomFilterPolicy::KeyMayMatch
  //
  // 约束：
  //   filter.size() < 2        -> 返回 false（空 filter 表示「什么都没有」）
  //   k = filter[-1]；k > 30   -> 返回 true（视为损坏：宁可多读一次盘）
  //   bits = (filter.size() - 1) * 8
  //   用与 CreateFilter 完全相同的 h1/h2 规则检查 k 个位，有一位是 0 就 false
  //
  // 注意「k 从哪读」：是最后一个字节，不是第一个。读错位置会让 k 变成
  // 位图的第 0 字节，几乎所有查询都返回 true —— 假阳性 100%，但测试
  // Bloom.NoFalseNegative 还是能过，只有 FprWithinBound 会挂。
  // -------------------------------------------------------------------------
  bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
    (void)key;
    (void)filter;
    // 占位实现：一律说「不在这里」。真实存在的 key 也会被挡住（假阴性），
    // 这在生产里是丢数据，测试里表现为「Put 过的 key 全查不到」。
    // 按上面 4 步填；注意 k 在 filter 的**最后一个字节**。
    return false;
  }

 private:
  size_t k_;
  int bits_per_key_;
};

}  // namespace

std::unique_ptr<const FilterPolicy> NewBloomFilterPolicy(int bits_per_key) {
  return std::unique_ptr<const FilterPolicy>(new BloomFilterPolicy(bits_per_key));
}

}  // namespace lsm
