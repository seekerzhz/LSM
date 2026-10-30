// FilterPolicy：块级「这个 key 在不在」的判定接口。
//
// 为什么需要它：一次 Get 要碰 N 个 SST，每个 SST 至少要读一个 data block。
// 有了 filter，绝大多数「不在这里」的判断可以在内存里做完，一次盘都不用读。
// 假阳性只浪费一次读，假阴性会**丢数据**——所以所有实现都必须偏向「说在」。
#pragma once

#include <memory>

#include "lsm/slice.h"

namespace lsm {

class FilterPolicy {
 public:
  virtual ~FilterPolicy() = default;

  // 写进 SST 的元数据里，用来识别 filter 类型。改了算法就要换名字，
  // 否则旧文件会被新代码用错误的规则解析。
  virtual const char* Name() const = 0;

  // 为一组 key 生成 filter 内容（不含长度前缀），追加到 dst。
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;

  // key 是否**可能**在 filter 里。返回 false 意味着「一定不在」。
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// 调用方拥有返回值（用 std::unique_ptr<const FilterPolicy> 接）。
// 不返回单例：不同 SST 可能用不同 bits_per_key，测试也会用不同参数对比。
std::unique_ptr<const FilterPolicy> NewBloomFilterPolicy(int bits_per_key);

}  // namespace lsm
