// 延迟直方图：按 2 的幂分桶。
//
// 为什么不存全部样本算精确分位：一次 5 分钟的压测有几百万个样本，
// 存下来要几十 MB，而 p99 这个数字本身只需要 ±3% 的精度。
// 对数桶的最大相对误差是 1/32（约 3%），够用了。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace lsm {

class LatencyHistogram {
 public:
  LatencyHistogram() { buckets_.assign(kNumBuckets, 0); }

  void Record(uint64_t micros) {
    buckets_[BucketIndex(micros)]++;
    ++count_;
    sum_ += micros;
    if (micros > max_) max_ = micros;
  }

  // p 取 50 / 90 / 99 / 99.9
  uint64_t Percentile(double p) const {
    if (count_ == 0) return 0;
    // 向上取整：p99 在 100 个样本时应落在第 99 个，不是第 100 个
    uint64_t target = static_cast<uint64_t>(static_cast<double>(count_) * p / 100.0 + 0.5);
    if (target < 1) target = 1;
    uint64_t acc = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      acc += buckets_[i];
      if (acc >= target) return BucketValue(i);
    }
    return max_;
  }

  uint64_t count() const { return count_; }
  uint64_t max() const { return max_; }
  double avg() const { return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_); }

 private:
  static constexpr size_t kNumBuckets = 40;  // 2^39 微秒 ≈ 15 小时，足够

  static size_t BucketIndex(uint64_t v) {
    size_t i = 0;
    while (v > 0) {
      v >>= 1;
      ++i;
    }
    return i < kNumBuckets ? i : kNumBuckets - 1;
  }

  static uint64_t BucketValue(size_t i) { return i == 0 ? 0 : (1ull << i); }

  std::vector<uint64_t> buckets_;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t max_ = 0;
};

}  // namespace lsm
