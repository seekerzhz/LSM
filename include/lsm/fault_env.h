// 故障注入 Env：让「第 N 次写失败」这件事可复现。
//
// 崩溃安全的测试没法靠「拔电源」做。可行的办法是：在每一个可能失败的点
// 注入一个错误，然后检查数据库还能不能打开、数据还在不在。
//
// 用法：
//   LSM_FAULT=5                第 5 次写操作失败
//   LSM_FAULT_OP=sync          只让 Sync 失败（默认 append）
//   LSM_FAULT_PERSIST=1        失败后持续失败（模拟磁盘彻底坏）
//   LSM_FAULT_RENAME=1         额外让 rename 失败（模拟原子性被破坏）
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "lsm/env.h"

namespace lsm {

class FaultInjectionEnv : public Env {
 public:
  // 从环境变量创建。没有任何 LSM_FAULT* 变量时返回 nullptr。
  // 返回指针由调用方拥有。
  static FaultInjectionEnv* CreateFromEnv(Env* target);

  explicit FaultInjectionEnv(Env* target);
  ~FaultInjectionEnv() override;

  // 直接设定规则（测试用，绕过环境变量）
  void SetFault(int fail_at, const std::string& op, bool persist);

  Status NewSequentialFile(const std::string& path,
                           std::unique_ptr<SequentialFile>* result) override;
  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* result) override;
  Status NewWritableFile(const std::string& path,
                         std::unique_ptr<WritableFile>* result) override;
  Status NewAppendableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override;

  bool FileExists(const std::string& path) override;
  Status GetFileSize(const std::string& path, uint64_t* size) override;
  Status RemoveFile(const std::string& path) override;
  Status RenameFile(const std::string& from, const std::string& to) override;
  Status CreateDir(const std::string& path) override;
  Status ListDir(const std::string& path, std::vector<std::string>* names) override;
  Status RemoveDir(const std::string& path) override;

  // 已经观测到多少次写操作（测试用来断言「确实注入到了」）
  int write_count() const { return writes_.load(std::memory_order_relaxed); }
  bool injected() const { return injected_.load(std::memory_order_relaxed); }

 private:
  friend class FaultWritableFile;

  // 返回 true 表示「这次应该失败」
  bool ShouldFail(const char* op);

  Env* target_;
  std::atomic<int> fail_at_{0};
  std::string fail_op_{"append"};
  bool persist_ = false;
  std::atomic<int> writes_{0};
  std::atomic<bool> injected_{false};
  std::atomic<bool> tripped_{false};
};

}  // namespace lsm
