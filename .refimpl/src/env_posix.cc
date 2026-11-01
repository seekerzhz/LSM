// POSIX 实现的 Env。
//
// 注意：本文件使用 <fcntl.h> / <unistd.h>，只能在 Linux / macOS / WSL2 上编译。
// Windows 原生（MSVC）不支持，请用 WSL2。
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lsm/env.h"

namespace lsm {
namespace {

// errno -> Status 的统一出口。
//
// ENOENT 必须映射成 NotFound 而不是 IOError：上层靠 IsNotFound() 区分
// 「这个文件还不存在」（全新库，正常）和「这个文件读不了」（磁盘坏了，要报错）。
// 全部当 IOError 的后果在本课程里很具体：第一次打开一个空目录时，
// VersionSet::Recover 读 CURRENT 拿到 IOError，于是不去建初始 MANIFEST，
// DB::Open 直接失败——而且错误信息看起来像权限问题，极难定位。
Status PosixError(const std::string& context, const std::string& path, int err) {
  if (err == ENOENT) {
    return Status::NotFound(context + " " + path + ": " + std::strerror(err));
  }
  return Status::IOError(context + " " + path + ": " + std::strerror(err));
}

// 带 EINTR 重试的 close
int CloseFd(int fd) {
  int r;
  do {
    r = ::close(fd);
  } while (r != 0 && errno == EINTR);
  return r;
}

class PosixSequentialFile : public SequentialFile {
 public:
  PosixSequentialFile(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {}
  ~PosixSequentialFile() override {
    if (fd_ >= 0) CloseFd(fd_);
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    ssize_t r;
    do {
      r = ::read(fd_, scratch, n);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return PosixError("read", fname_, errno);
    *result = Slice(scratch, static_cast<size_t>(r));
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError("lseek", fname_, errno);
    }
    return Status::OK();
  }

 private:
  std::string fname_;
  int fd_;
};

class PosixRandomAccessFile : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) CloseFd(fd_);
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    ssize_t r;
    do {
      r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    } while (r < 0 && errno == EINTR);
    if (r < 0) return PosixError("pread", fname_, errno);
    *result = Slice(scratch, static_cast<size_t>(r));
    return Status::OK();
  }

 private:
  std::string fname_;
  int fd_;
};

// 用户态缓冲大小：超过就自动 Flush 到内核
constexpr size_t kWriteBufferSize = 64 * 1024;

class PosixWritableFile : public WritableFile {
 public:
  PosixWritableFile(std::string fname, int fd)
      : fname_(std::move(fname)), fd_(fd), size_(0) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      Flush();  // 尽力而为：析构不能报错
      CloseFd(fd_);
    }
  }

  // -------------------------------------------------------------------------
  // TODO(0.6) Append 的缓冲策略
  //
  // 输入：data
  // 输出：追加到 buf_；当 buf_ 超过 kWriteBufferSize 时调用 Flush() 写进内核
  // 约束：Append 返回 ok 时，数据**只保证在 buf_ 里**（可能在用户态内存）。
  //       进程被 kill -9 时这部分数据会丢 —— 第 1 章的 WAL 依赖这个结论。
  // -------------------------------------------------------------------------
  Status Append(const Slice& data) override {
    // 课程提供的基础版本（完整，无需修改）：
    buf_.append(data.data(), data.size());
    size_ += data.size();
    if (buf_.size() >= kWriteBufferSize) return Flush();
    return Status::OK();
  }

  Status Flush() override {
    if (buf_.empty()) return Status::OK();
    size_t written = 0;
    while (written < buf_.size()) {
      ssize_t r = ::write(fd_, buf_.data() + written, buf_.size() - written);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError("write", fname_, errno);
      }
      written += static_cast<size_t>(r);
    }
    buf_.clear();
    return Status::OK();
  }

  // -------------------------------------------------------------------------
  // TODO(0.5) Sync
  //
  // 输入：无
  // 输出：先把 buf_ 刷进内核（调 Flush），再调 ::fsync(fd_)
  // 约束：
  //   - fsync 返回 EINTR 时必须重试（否则一次信号就让写入静默失败）
  //   - 失败时返回 IOError，消息里带 strerror(errno)
  //   - 必须用 fsync 而不是 fdatasync（本课程要求元数据也落盘）
  // -------------------------------------------------------------------------
  Status Sync() override {
    Status s = Flush();
    if (!s.ok()) return s;
    // fsync 也会被信号打断。EINTR 直接当失败返回的话，一次无关的信号
    // 就能让「写成功」变成「返回错误但数据其实已经在盘上」——这种
    // 偶发的假失败会让人怀疑整套 WAL，所以必须重试到真的出结果。
    int r;
    do {
      r = ::fsync(fd_);
    } while (r != 0 && errno == EINTR);
    if (r != 0) return PosixError("fsync", fname_, errno);
    return Status::OK();
  }

  uint64_t Size() const override { return size_; }

 private:
  std::string fname_;
  int fd_;
  std::string buf_;
  uint64_t size_;
};

class PosixEnv : public Env {
 public:
  Status NewSequentialFile(const std::string& path,
                           std::unique_ptr<SequentialFile>* result) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError("open", path, errno);
    *result = std::make_unique<PosixSequentialFile>(path, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* result) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError("open", path, errno);
    *result = std::make_unique<PosixRandomAccessFile>(path, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& path,
                         std::unique_ptr<WritableFile>* result) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError("open", path, errno);
    *result = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError("open", path, errno);
    uint64_t sz = 0;
    Status s = GetFileSize(path, &sz);
    if (!s.ok()) {
      CloseFd(fd);
      return s;
    }
    *result = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& path) override {
    return ::access(path.c_str(), F_OK) == 0;
  }

  Status GetFileSize(const std::string& path, uint64_t* size) override {
    struct stat sb;
    if (::stat(path.c_str(), &sb) != 0) return PosixError("stat", path, errno);
    *size = static_cast<uint64_t>(sb.st_size);
    return Status::OK();
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) {
      if (errno == ENOENT) return Status::OK();  // 已不存在视为成功（幂等）
      return PosixError("unlink", path, errno);
    }
    return Status::OK();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      return PosixError("rename " + from + " ->", to, errno);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) != 0) {
      if (errno == EEXIST) return Status::OK();
      return PosixError("mkdir", path, errno);
    }
    return Status::OK();
  }

  Status ListDir(const std::string& path, std::vector<std::string>* names) override {
    DIR* d = ::opendir(path.c_str());
    if (d == nullptr) return PosixError("opendir", path, errno);
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
      const std::string name = ent->d_name;
      if (name == "." || name == "..") continue;
      names->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status RemoveDir(const std::string& path) override {
    if (::rmdir(path.c_str()) != 0) {
      if (errno == ENOENT) return Status::OK();
      return PosixError("rmdir", path, errno);
    }
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace lsm
