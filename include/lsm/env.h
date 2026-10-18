// 文件系统抽象。
//
// 为什么要有这一层：第 3/4 章要测"写入过程中崩溃"，如果代码里到处是裸
// open/write，你无法注入故障。抽出 Env 后可以写 FaultInjectionEnv
// 让第 N 次 Append 失败，从而验证崩溃安全性。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// 只读、顺序读的文件。
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // 读取最多 n 字节，把结果写进 *result（指向 scratch 或内部缓冲）。
  // 返回实际读到的字节数在 result->size() 里；读到 EOF 时 result 为空且 status ok。
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
  // 跳过 n 字节。越界时返回 ok 并停在 EOF。
  virtual Status Skip(uint64_t n) = 0;
};

// 随机读的文件（SST 用）。
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  // 从 offset 读最多 n 字节。
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

// 只写、追加写的文件。
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  // 追加 data。**只写进用户态缓冲或内核缓冲，不保证落盘。**
  virtual Status Append(const Slice& data) = 0;
  // 把用户态缓冲写进内核（write(2)）。
  virtual Status Flush() = 0;
  // 真正落盘（fsync(2)）。**返回 ok 才意味着"这条数据断电也不丢"。**
  virtual Status Sync() = 0;
  // 当前文件长度（含未 Flush 的缓冲）。
  virtual uint64_t Size() const = 0;
};

class Env {
 public:
  virtual ~Env() = default;
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& path,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& path,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  virtual Status NewWritableFile(const std::string& path,
                                 std::unique_ptr<WritableFile>* result) = 0;
  // 追加模式打开（WAL 恢复后用）。
  virtual Status NewAppendableFile(const std::string& path,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& path) = 0;
  virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;
  virtual Status RemoveFile(const std::string& path) = 0;
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;
  virtual Status CreateDir(const std::string& path) = 0;
  virtual Status ListDir(const std::string& path, std::vector<std::string>* names) = 0;
  virtual Status RemoveDir(const std::string& path) = 0;
};

}  // namespace lsm
