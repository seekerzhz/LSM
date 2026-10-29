// 测试辅助（最底层的那些）：定宽键、值、internal key 的构造。
//
// 特意和 test_util.h 分开：这里面只依赖 dbformat.h 和 env.h，
// 所以阶段 1 的跳表/MemTable 测试、阶段 2 的 Block/Bloom 测试都能直接用，
// 不需要等 SSTable 那一层写完。
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "lsm/dbformat.h"
#include "lsm/env.h"

namespace lsm_test {

// 每次都从一个空目录开始。名字固定，便于失败后去 /tmp 看现场。
inline std::string FreshDir(const std::string& name) {
  lsm::Env* env = lsm::Env::Default();
  const std::string root = "/tmp/lsm_course_test";
  env->CreateDir(root);
  const std::string dir = root + "/" + name;

  std::vector<std::string> names;
  if (env->ListDir(dir, &names).ok()) {
    for (const std::string& n : names) env->RemoveFile(dir + "/" + n);
    env->RemoveDir(dir);
  }
  env->CreateDir(dir);
  return dir;
}

// 把整数补成定宽字符串：保证 "key9" < "key10" 的字典序与数值序一致。
// 不补零会让有序性测试出现「看起来对、其实靠运气」的结果。
inline std::string Padded(uint64_t i, int width = 8) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%0*llu", width, static_cast<unsigned long long>(i));
  return std::string(buf);
}

inline std::string Key(uint64_t i) { return "key" + Padded(i); }

inline std::string Value(uint64_t i, size_t len = 32) {
  std::string v = "value" + Padded(i) + ":";
  while (v.size() < len) v.push_back('x');
  v.resize(len);
  return v;
}

// 构造 internal key 的原始字节
inline std::string IKey(const std::string& user_key, lsm::SequenceNumber seq,
                        lsm::ValueType type = lsm::kTypeValue) {
  std::string out;
  lsm::AppendInternalKey(&out, lsm::ParsedInternalKey{lsm::Slice(user_key), seq, type});
  return out;
}

inline std::string UserOf(const lsm::Slice& internal_key) {
  return lsm::ExtractUserKey(internal_key).ToString();
}

}  // namespace lsm_test
