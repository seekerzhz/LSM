// 极简 assert runner 的 main。只在没有 GoogleTest 时编译。
//
// 用法：
//   ./lsm_tests                 跑全部
//   ./lsm_tests --filter=Coding 只跑名字以 Coding 开头的用例（前缀匹配）
//   ./lsm_tests Coding          同上（位置参数形式）
//   ./lsm_tests --list          列出所有用例
#include <cstdio>
#include <cstring>
#include <string>

#include "tests/test_macros.h"

#ifndef LSM_NO_GTEST

// 用 gtest_main 时这个文件不会被编译，这里只是占位避免空翻译单元警告
namespace {
struct Unused {
  int x;
};
}  // namespace

#else

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--list") {
      for (const auto& c : lsm_test::All()) std::printf("%s\n", c.name);
      return 0;
    }
    const std::string prefix = "--filter=";
    if (a.compare(0, prefix.size(), prefix) == 0) {
      filter = a.substr(prefix.size());
    } else if (a.size() > 1 && a[0] != '-') {
      filter = a;
    }
  }
  lsm_test::FilterPrefix() = filter.c_str();

  int run = 0;
  for (const auto& c : lsm_test::All()) {
    if (!lsm_test::Match(c.name)) continue;
    ++run;
    const int before = lsm_test::Failures();
    std::printf("[ RUN  ] %s\n", c.name);
    // 必须 flush：某个用例在库代码里 assert/segfault 时进程直接死掉，
    // stdout 的缓冲区跟着没了，你会看到「什么都没打印就退出了」，
    // 连是哪个用例崩的都查不到。这一行 fflush 就是为了留下现场。
    std::fflush(stdout);
    c.fn();
    if (lsm_test::Failures() == before) {
      std::printf("[  OK  ] %s\n", c.name);
    } else {
      std::printf("[ FAIL ] %s\n", c.name);
    }
    std::fflush(stdout);
  }

  std::printf("=== %d cases, %d checks, %d failures ===\n", run,
              lsm_test::Checks(), lsm_test::Failures());
  return lsm_test::Failures() == 0 ? 0 : 1;
}

#endif  // LSM_NO_GTEST
