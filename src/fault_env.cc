#include "lsm/fault_env.h"

#include <cstdlib>
#include <cstring>

namespace lsm {
namespace {

bool ReadIntEnv(const char* name, int* out) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return false;
  *out = std::atoi(v);
  return true;
}

bool ReadStrEnv(const char* name, std::string* out) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return false;
  *out = v;
  return true;
}

}  // namespace

// 包装一个 WritableFile，在每个写操作前问一句「这次该失败吗」
class FaultWritableFile : public WritableFile {
 public:
  FaultWritableFile(std::unique_ptr<WritableFile> target, FaultInjectionEnv* env)
      : target_(std::move(target)), env_(env) {}

  Status Append(const Slice& data) override {
    if (env_->ShouldFail("append")) {
      // 模拟「写了一半」：先让一小段真的写进去，再报错。
      // 这样测试能覆盖「文件尾部有半条记录」这种真实的崩溃形态。
      if (data.size() > 8) target_->Append(Slice(data.data(), 8));
      return Status::IOError("injected fault: append");
    }
    return target_->Append(data);
  }

  Status Flush() override {
    if (env_->ShouldFail("flush")) return Status::IOError("injected fault: flush");
    return target_->Flush();
  }

  Status Sync() override {
    if (env_->ShouldFail("sync")) return Status::IOError("injected fault: sync");
    return target_->Sync();
  }

  uint64_t Size() const override { return target_->Size(); }

 private:
  std::unique_ptr<WritableFile> target_;
  FaultInjectionEnv* env_;
};

FaultInjectionEnv* FaultInjectionEnv::CreateFromEnv(Env* target) {
  int fail_at = 0;
  if (!ReadIntEnv("LSM_FAULT", &fail_at) || fail_at <= 0) return nullptr;

  auto* env = new FaultInjectionEnv(target);
  std::string op = "append";
  ReadStrEnv("LSM_FAULT_OP", &op);
  const bool persist = std::getenv("LSM_FAULT_PERSIST") != nullptr;
  env->SetFault(fail_at, op, persist);
  return env;
}

FaultInjectionEnv::FaultInjectionEnv(Env* target) : target_(target) {}

FaultInjectionEnv::~FaultInjectionEnv() = default;

void FaultInjectionEnv::SetFault(int fail_at, const std::string& op, bool persist) {
  fail_at_.store(fail_at, std::memory_order_relaxed);
  fail_op_ = op;
  persist_ = persist;
}

bool FaultInjectionEnv::ShouldFail(const char* op) {
  // 只数「写类」操作，读操作不参与计数，这样 LSM_FAULT=5 在一次
  // 「先读后写」的流程里指向的是第 5 次写，而不是第 5 次调用。
  const int n = writes_.fetch_add(1, std::memory_order_relaxed) + 1;
  const int limit = fail_at_.load(std::memory_order_relaxed);
  if (limit <= 0) return false;

  const bool op_matches = (fail_op_ == op);
  const bool hit = (n >= limit) && (persist_ ? true : n == limit) && op_matches;
  if (hit) injected_.store(true, std::memory_order_relaxed);
  if (tripped_.load(std::memory_order_relaxed) && persist_) return op_matches;
  if (hit) tripped_.store(true, std::memory_order_relaxed);
  return hit;
}

Status FaultInjectionEnv::NewSequentialFile(const std::string& path,
                                            std::unique_ptr<SequentialFile>* result) {
  return target_->NewSequentialFile(path, result);
}

Status FaultInjectionEnv::NewRandomAccessFile(const std::string& path,
                                              std::unique_ptr<RandomAccessFile>* result) {
  return target_->NewRandomAccessFile(path, result);
}

Status FaultInjectionEnv::NewWritableFile(const std::string& path,
                                          std::unique_ptr<WritableFile>* result) {
  std::unique_ptr<WritableFile> real;
  Status s = target_->NewWritableFile(path, &real);
  if (!s.ok()) return s;
  *result = std::make_unique<FaultWritableFile>(std::move(real), this);
  return Status::OK();
}

Status FaultInjectionEnv::NewAppendableFile(const std::string& path,
                                            std::unique_ptr<WritableFile>* result) {
  std::unique_ptr<WritableFile> real;
  Status s = target_->NewAppendableFile(path, &real);
  if (!s.ok()) return s;
  *result = std::make_unique<FaultWritableFile>(std::move(real), this);
  return Status::OK();
}

bool FaultInjectionEnv::FileExists(const std::string& path) {
  return target_->FileExists(path);
}
Status FaultInjectionEnv::GetFileSize(const std::string& path, uint64_t* size) {
  return target_->GetFileSize(path, size);
}
Status FaultInjectionEnv::RemoveFile(const std::string& path) {
  return target_->RemoveFile(path);
}
Status FaultInjectionEnv::RenameFile(const std::string& from, const std::string& to) {
  // rename 的故障不参与通用计数：它是「原子性」这条性质的关键点，
  // 需要独立开关（LSM_FAULT_OP=rename）。
  if (fail_op_ == "rename" && ShouldFail("rename")) {
    return Status::IOError("injected fault: rename");
  }
  return target_->RenameFile(from, to);
}
Status FaultInjectionEnv::CreateDir(const std::string& path) {
  return target_->CreateDir(path);
}
Status FaultInjectionEnv::ListDir(const std::string& path, std::vector<std::string>* names) {
  return target_->ListDir(path, names);
}
Status FaultInjectionEnv::RemoveDir(const std::string& path) {
  return target_->RemoveDir(path);
}

}  // namespace lsm
