// Status: 错误码 + 消息。本课程不用异常。
//
// 理由：存储引擎里「文件不存在」「磁盘满」「数据损坏」是常态而非异常，
// 异常会让错误处理路径隐藏起来，读者看不出哪些调用可能失败。
#pragma once

#include <string>

#include "lsm/slice.h"

namespace lsm {

enum class Code {
  kOk = 0,
  kNotFound = 1,
  kCorruption = 2,
  kIOError = 3,
  kInvalidArgument = 4,
};

class Status {
 public:
  Status() : code_(Code::kOk) {}

  static Status OK() { return Status(); }
  static Status NotFound(const Slice& msg) { return Status(Code::kNotFound, msg); }
  static Status Corruption(const Slice& msg) { return Status(Code::kCorruption, msg); }
  static Status IOError(const Slice& msg) { return Status(Code::kIOError, msg); }
  static Status Invalid(const Slice& msg) { return Status(Code::kInvalidArgument, msg); }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }
  std::string ToString() const;

 private:
  Status(Code c, const Slice& msg) : code_(c), msg_(msg.ToString()) {}
  Code code_;
  std::string msg_;
};

}  // namespace lsm
