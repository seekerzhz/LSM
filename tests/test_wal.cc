#include "lsm/log_reader.h"
#include "lsm/log_writer.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lsm/env.h"
#include "tests/test_macros.h"

namespace lsm {
namespace {

std::string TempPath(const std::string& tag) {
  static int counter = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = std::filesystem::temp_directory_path();
  return (dir / ("lsm_wal_" + tag + "_" + std::to_string(now) + "_" +
                 std::to_string(counter++)))
      .string();
}

// 写 n 条记录到 path，每条内容为 "record-<i>"
void WriteRecords(const std::string& path, int n, size_t pad = 0) {
  Env* env = Env::Default();
  std::unique_ptr<WritableFile> f;
  Status s = env->NewWritableFile(path, &f);
  ASSERT_TRUE(s.ok());
  log::Writer writer(f.get());
  for (int i = 0; i < n; ++i) {
    std::string rec = "record-" + std::to_string(i);
    if (pad > 0) rec.append(pad, 'x');
    s = writer.AddRecord(Slice(rec));
    ASSERT_TRUE(s.ok());
  }
  ASSERT_TRUE(f->Sync().ok());
}

std::vector<std::string> ReadAll(const std::string& path, Status* final_status) {
  Env* env = Env::Default();
  std::unique_ptr<SequentialFile> f;
  Status s = env->NewSequentialFile(path, &f);
  EXPECT_TRUE(s.ok());
  log::Reporter reporter;
  reporter.status = final_status;
  reporter.fname = nullptr;
  log::Reader reader(f.get(), &reporter, true);
  std::vector<std::string> out;
  Slice rec;
  std::string scratch;
  while (reader.ReadRecord(&rec, &scratch)) {
    out.push_back(rec.ToString());
  }
  return out;
}

TEST(Wal, RoundTrip100) {
  const std::string path = TempPath("rt100");
  WriteRecords(path, 100);

  Status st;
  const std::vector<std::string> got = ReadAll(path, &st);
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(got.size() == 100u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(got[static_cast<size_t>(i)] == "record-" + std::to_string(i));
  }
  std::filesystem::remove(path);
}

TEST(Wal, CrossesBlockBoundary) {
  const std::string path = TempPath("cross");
  // 每条 ~1000 字节，50 条约 50KB，必然跨过 32KB 块边界
  WriteRecords(path, 50, 1000);

  Status st;
  const std::vector<std::string> got = ReadAll(path, &st);
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(50, static_cast<int>(got.size()));
  std::filesystem::remove(path);
}

TEST(Wal, TruncatedLastRecordIsDropped) {
  const std::string path = TempPath("trunc");
  WriteRecords(path, 10);

  // 砍掉最后 3 字节，模拟"写到一半被 kill -9"
  // 用 error_code 版本，避免文件是空的时候 resize 抛异常打断整个测试进程
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > 3) {
    std::filesystem::resize_file(path, size - 3, ec);
  }

  Status st;
  const std::vector<std::string> got = ReadAll(path, &st);
  // 允许丢掉最后一条，但前面 9 条必须完好，且不能报 Corruption
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(got.size() >= 9u && got.size() <= 10u);
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_TRUE(got[i] == "record-" + std::to_string(i));
  }
  std::filesystem::remove(path, ec);
}

TEST(Wal, RecordTooLargeRejected) {
  const std::string path = TempPath("toolarge");
  Env* env = Env::Default();
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
  log::Writer writer(f.get());

  const std::string huge(log::kMaxRecordPayload + 1, 'z');
  const Status s = writer.AddRecord(Slice(huge));
  EXPECT_FALSE(s.ok());
  std::filesystem::remove(path);
}

TEST(Wal, EmptyRecordOk) {
  const std::string path = TempPath("empty");
  Env* env = Env::Default();
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env->NewWritableFile(path, &f).ok());
  log::Writer writer(f.get());
  ASSERT_TRUE(writer.AddRecord(Slice("")).ok());
  ASSERT_TRUE(writer.AddRecord(Slice("after")).ok());
  ASSERT_TRUE(f->Sync().ok());

  Status st;
  const std::vector<std::string> got = ReadAll(path, &st);
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(got.size() == 2u);
  EXPECT_TRUE(got[0].empty());
  EXPECT_TRUE(got[1] == "after");
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace lsm
