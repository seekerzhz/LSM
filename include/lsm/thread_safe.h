// RefCounted：引用计数基类。
//
// 本课程需要它的只有两处（MemTable、Version），但这两处的引用计数
// 一旦不对称就是 use-after-free 或内存泄漏，而且都是「偶发、难复现」。
// 所以把 Ref/Unref 抽出来，只写一遍，并在调试构建里断言。
#pragma once

#include <atomic>
#include <cassert>

namespace lsm {

class RefCounted {
 public:
  RefCounted() = default;
  virtual ~RefCounted() = default;

  RefCounted(const RefCounted&) = delete;
  RefCounted& operator=(const RefCounted&) = delete;

  // 引用计数用 relaxed 就够了：「对象存在」这件事已经由持有者保证。
  // 但 Unref 的递减要用 acq_rel，因为「最后一个 Unref」必须看到
  // 其他线程在此之前对这个对象的所有写入（否则 delete 时可能看到半成品）。
  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }

  void Unref() {
    const int prev = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(prev > 0 && "RefCounted::Unref on an object with refs == 0");
    if (prev == 1) {
      OnZeroRefs();
      delete this;
    }
  }

  int refs() const { return refs_.load(std::memory_order_relaxed); }

 protected:
  // 子类可以在这里做「最后一个引用消失前的清理」（比如删文件）
  virtual void OnZeroRefs() {}

 private:
  std::atomic<int> refs_{0};
};

}  // namespace lsm
