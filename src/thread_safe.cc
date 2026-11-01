// 这个文件没有实现体：RefCounted 全部是 inline 的。
// 保留它是因为 CMakeLists 里列了 src/thread_safe.cc，
// 而「头文件里的模板/内联函数 + 一个空翻译单元」是常见做法：
// 保证任何包含该头文件的编译单元都能独立通过语法检查。
#include "lsm/thread_safe.h"

namespace lsm {
namespace {
// 防止空翻译单元被某些编译器警告
[[maybe_unused]] const int kThreadSafeVersion = 1;
}  // namespace
}  // namespace lsm
