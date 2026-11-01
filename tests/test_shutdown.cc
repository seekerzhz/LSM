// 阶段 4：关闭。这是整门课最容易「看起来没问题」的一段代码。
//
// 关闭要做四件事，顺序不能错，漏一件就有一类稳定的故障：
//   1. 置 shutting_down_ 并通知后台线程 —— 漏了，join 永远等不到
//   2. join 后台线程 —— 漏了，线程还在用已经析构的 this
//   3. 最后那个 MemTable 落盘 —— 漏了，最近几秒的写入静默消失
//   4. 释放文件句柄 —— 漏了，测不出来，直到第 1021 次 open 报 EMFILE
//
// 第 4 条是最阴的：它不报错、不崩溃、不影响正确性，只在跑久了之后突然打不开文件。
// 所以这里用一个专门的用例去数 /proc/self/fd。
#include <dirent.h>

#include <memory>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "lsm/env.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Value;

namespace {

Options SmallBuffer(const std::string& dir) {
  Options o;
  o.db_path = dir;
  o.write_buffer_size = 32u << 10;
  o.max_file_size = 64u << 10;
  o.l0_compaction_trigger = 2;
  return o;
}

DB* OpenDb(const Options& o) {
  DB* db = nullptr;
  if (!DB::Open(o, &db).ok()) return nullptr;
  return db;
}

// 本进程当前打开的文件描述符个数。读不到就返回 -1（非 Linux 环境）。
int CountOpenFds() {
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) return -1;
  int n = 0;
  while (::readdir(d) != nullptr) ++n;
  ::closedir(d);
  return n;
}

void WriteRange(DB* db, uint64_t lo, uint64_t hi) {
  const WriteOptions w;
  for (uint64_t i = lo; i < hi; ++i) {
    const Status s = db->Put(w, Slice(Key(i)), Slice(Value(i)));
    if (!s.ok()) {
      std::fprintf(stderr, "    put %s failed: %s\n", Key(i).c_str(), s.ToString().c_str());
      return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Close 必须可重入：析构函数会自动调一次，用户显式调过之后又来一次很常见
// ---------------------------------------------------------------------------
TEST(Shutdown, CloseIsIdempotent) {
  const Options o = SmallBuffer(lsm_test::FreshDir("shut_idem"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  ASSERT_TRUE(db->Put(WriteOptions(), Slice("k"), Slice("v")).ok());
  EXPECT_TRUE(db->Close().ok());
  EXPECT_TRUE(db->Close().ok());  // 第二次不能崩、不能返回错误
  delete db;                      // 析构里还有第三次
}

// ---------------------------------------------------------------------------
// 一次 Close 都没调，直接 delete：析构里那条路径同样要能把数据保住
// ---------------------------------------------------------------------------
TEST(Shutdown, DestructorPersistsData) {
  const std::string dir = lsm_test::FreshDir("shut_dtor");
  {
    DB* db = OpenDb(SmallBuffer(dir));
    ASSERT_TRUE(db != nullptr);
    WriteRange(db, 0, 300);
    delete db;  // 不调 Close
  }

  DB* db = OpenDb(SmallBuffer(dir));
  ASSERT_TRUE(db != nullptr);
  int missing = 0;
  std::string v;
  for (uint64_t i = 0; i < 300; ++i) {
    if (!db->Get(ReadOptions(), Slice(Key(i)), &v).ok()) ++missing;
  }
  if (missing != 0) std::fprintf(stderr, "    未 Close 直接析构，丢了 %d 条\n", missing);
  EXPECT_EQ(missing, 0);
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 关闭时 compaction 正在跑：必须等它收尾，而不是把文件删到一半就走
// ---------------------------------------------------------------------------
TEST(Shutdown, CloseWhileCompactionInFlight) {
  const std::string dir = lsm_test::FreshDir("shut_inflight");

  {
    DB* db = OpenDb(SmallBuffer(dir));
    ASSERT_TRUE(db != nullptr);
    WriteRange(db, 0, 800);
    // 不等 flush、不等 compaction，直接关。后台线程此刻很可能正在写盘
    EXPECT_TRUE(db->Close().ok());
    delete db;
  }

  DB* db = OpenDb(SmallBuffer(dir));
  ASSERT_TRUE(db != nullptr);
  int missing = 0;
  std::string v;
  for (uint64_t i = 0; i < 800; ++i) {
    if (!db->Get(ReadOptions(), Slice(Key(i)), &v).ok()) ++missing;
  }
  if (missing != 0) std::fprintf(stderr, "    带着 compaction 关闭，丢了 %d 条\n", missing);
  EXPECT_EQ(missing, 0);
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 句柄泄漏：开开关关十次，文件描述符数量必须回到原位
// ---------------------------------------------------------------------------
TEST(Shutdown, NoFdLeakAcrossReopen) {
  const std::string dir = lsm_test::FreshDir("shut_fd");

  // 先跑一遍让 table_cache / manifest 之类的"一次性"打开都发生，
  // 否则第一次测量会把正常的常驻句柄算成泄漏
  {
    DB* db = OpenDb(SmallBuffer(dir));
    ASSERT_TRUE(db != nullptr);
    WriteRange(db, 0, 200);
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }

  const int before = CountOpenFds();
  ASSERT_TRUE(before > 0);

  for (int round = 0; round < 10; ++round) {
    DB* db = OpenDb(SmallBuffer(dir));
    ASSERT_TRUE(db != nullptr);
    WriteRange(db, static_cast<uint64_t>(round) * 10, static_cast<uint64_t>(round) * 10 + 10);
    ASSERT_TRUE(db->Flush().ok());
    std::string v;
    ASSERT_TRUE(db->Get(ReadOptions(), Slice(Key(0)), &v).ok());
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }

  const int after = CountOpenFds();
  if (after - before > 2) {
    std::fprintf(stderr, "    fd 从 %d 涨到 %d，疑似句柄泄漏\n", before, after);
  }
  EXPECT_TRUE(after - before <= 2);
}

// ---------------------------------------------------------------------------
// 反复开关 + 每轮都写：数据要累积，不能"重开后只剩最后一轮"
// ---------------------------------------------------------------------------
TEST(Shutdown, ReopenAccumulatesData) {
  const std::string dir = lsm_test::FreshDir("shut_accumulate");

  const int kRounds = 5;
  const uint64_t kPerRound = 100;
  for (int round = 0; round < kRounds; ++round) {
    DB* db = OpenDb(SmallBuffer(dir));
    ASSERT_TRUE(db != nullptr);
    const uint64_t base = static_cast<uint64_t>(round) * kPerRound;
    WriteRange(db, base, base + kPerRound);
    ASSERT_TRUE(db->Flush().ok());
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }

  DB* db = OpenDb(SmallBuffer(dir));
  ASSERT_TRUE(db != nullptr);
  int missing = 0;
  std::string v;
  for (uint64_t i = 0; i < kRounds * kPerRound; ++i) {
    if (!db->Get(ReadOptions(), Slice(Key(i)), &v).ok()) ++missing;
  }
  if (missing != 0) std::fprintf(stderr, "    五轮累积后丢了 %d 条\n", missing);
  EXPECT_EQ(missing, 0);
  EXPECT_TRUE(db->Close().ok());
  delete db;
}
