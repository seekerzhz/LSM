#include "lsm/env.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "tests/test_macros.h"

namespace lsm {
namespace {

std::string TempPath(const std::string& tag) {
  static int counter = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = std::filesystem::temp_directory_path();
  return (dir / ("lsm_env_" + tag + "_" + std::to_string(now) + "_" +
                 std::to_string(counter++)))
      .string();
}

TEST(Env, WriteReadBack) {
  Env* env = Env::Default();
  const std::string path = TempPath("rw");

  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(Slice("hello ")).ok());
    ASSERT_TRUE(f->Append(Slice("world")).ok());
    ASSERT_TRUE(f->Flush().ok());
    EXPECT_EQ(11u, f->Size());
  }

  uint64_t size = 0;
  ASSERT_TRUE(env->GetFileSize(path, &size).ok());
  EXPECT_EQ(11u, size);

  std::unique_ptr<SequentialFile> r;
  ASSERT_TRUE(env->NewSequentialFile(path, &r).ok());
  char buf[64];
  Slice result;
  ASSERT_TRUE(r->Read(64, &result, buf).ok());
  EXPECT_TRUE(result == Slice("hello world"));

  env->RemoveFile(path);
}

TEST(Env, SyncPersists) {
  Env* env = Env::Default();
  const std::string path = TempPath("sync");

  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(Slice("durable")).ok());
    // Sync 返回 ok 才意味着"断电也不丢"
    ASSERT_TRUE(f->Sync().ok());
  }

  std::unique_ptr<SequentialFile> r;
  ASSERT_TRUE(env->NewSequentialFile(path, &r).ok());
  char buf[64];
  Slice result;
  ASSERT_TRUE(r->Read(64, &result, buf).ok());
  EXPECT_TRUE(result == Slice("durable"));

  env->RemoveFile(path);
}

TEST(Env, AppendModeKeepsOldContent) {
  Env* env = Env::Default();
  const std::string path = TempPath("append");

  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(Slice("AAA")).ok());
    ASSERT_TRUE(f->Sync().ok());
  }
  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env->NewAppendableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(Slice("BBB")).ok());
    ASSERT_TRUE(f->Sync().ok());
  }

  uint64_t size = 0;
  ASSERT_TRUE(env->GetFileSize(path, &size).ok());
  EXPECT_EQ(6u, size);

  env->RemoveFile(path);
}

TEST(Env, RemoveMissingIsOk) {
  Env* env = Env::Default();
  // 幂等：删一个不存在的文件应返回 ok
  EXPECT_TRUE(env->RemoveFile(TempPath("missing")).ok());
  EXPECT_FALSE(env->FileExists(TempPath("missing")));
}

TEST(Env, RandomAccessRead) {
  Env* env = Env::Default();
  const std::string path = TempPath("rand");
  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(Slice("0123456789")).ok());
    ASSERT_TRUE(f->Sync().ok());
  }
  std::unique_ptr<RandomAccessFile> f;
  ASSERT_TRUE(env->NewRandomAccessFile(path, &f).ok());
  char buf[8];
  Slice result;
  ASSERT_TRUE(f->Read(3, 4, &result, buf).ok());
  EXPECT_TRUE(result == Slice("3456"));
  env->RemoveFile(path);
}

}  // namespace
}  // namespace lsm
