// 双模式测试宏。
//
// 定义了 LSM_NO_GTEST 时用内置的极简 runner；否则用 GoogleTest。
// 两种模式下写法完全一样，所以课程代码不依赖是否装了 GoogleTest。
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#ifdef LSM_NO_GTEST

namespace lsm_test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& All() {
  static std::vector<Case> v;
  return v;
}

inline int& Failures() {
  static int f = 0;
  return f;
}

inline int& Checks() {
  static int c = 0;
  return c;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { All().push_back(Case{n, f}); }
};

// 支持 --filter=X 前缀过滤
inline const char*& FilterPrefix() {
  static const char* p = "";
  return p;
}

inline bool Match(const char* name) {
  const std::string n(name);
  const std::string f(FilterPrefix());
  if (f.empty()) return true;
  return n.compare(0, f.size(), f) == 0;
}

}  // namespace lsm_test

#define LSM_CONCAT_(a, b) a##b
#define LSM_CONCAT(a, b) LSM_CONCAT_(a, b)

#define TEST(suite, name)                                            \
  static void LSM_CONCAT(suite, LSM_CONCAT(_, LSM_CONCAT(name, _impl)))();  \
  static ::lsm_test::Registrar LSM_CONCAT(suite, LSM_CONCAT(_, LSM_CONCAT(name, _reg)))( \
      #suite "." #name,                                              \
      LSM_CONCAT(suite, LSM_CONCAT(_, LSM_CONCAT(name, _impl))));    \
  static void LSM_CONCAT(suite, LSM_CONCAT(_, LSM_CONCAT(name, _impl)))()

#define EXPECT_TRUE(x)                                                        \
  do {                                                                        \
    ++::lsm_test::Checks();                                                   \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "    FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);   \
      ++::lsm_test::Failures();                                               \
    }                                                                         \
  } while (0)

#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))

#define EXPECT_EQ(a, b)                                                       \
  do {                                                                        \
    ++::lsm_test::Checks();                                                   \
    const auto& _a = (a);                                                     \
    const auto& _b = (b);                                                     \
    if (!(_a == _b)) {                                                        \
      std::fprintf(stderr, "    FAIL %s:%d  %s == %s\n", __FILE__, __LINE__,  \
                   #a, #b);                                                   \
      ++::lsm_test::Failures();                                               \
    }                                                                         \
  } while (0)

#define EXPECT_NE(a, b)                                                       \
  do {                                                                        \
    ++::lsm_test::Checks();                                                   \
    const auto& _a = (a);                                                     \
    const auto& _b = (b);                                                     \
    if (_a == _b) {                                                           \
      std::fprintf(stderr, "    FAIL %s:%d  %s != %s\n", __FILE__, __LINE__,  \
                   #a, #b);                                                   \
      ++::lsm_test::Failures();                                               \
    }                                                                         \
  } while (0)

// 致命断言：失败后直接 return
#define ASSERT_TRUE(x)                                                        \
  do {                                                                        \
    ++::lsm_test::Checks();                                                   \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "    ABORT %s:%d  %s\n", __FILE__, __LINE__, #x);  \
      ++::lsm_test::Failures();                                               \
      return;                                                                 \
    }                                                                         \
  } while (0)

#else  // 使用 GoogleTest

#include <gtest/gtest.h>

#endif
