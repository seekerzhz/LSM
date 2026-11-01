// 阶段 4：多线程。这组测试专门抓「单线程永远跑不出来」的那类 bug。
//
// 写这组测试的动机不是覆盖率，是复现真实故障：课程实现里出现过的丢数据，
// 全部发生在两个线程同时分配文件编号 / 同时刷 imm_ / 一边 compaction
// 一边删文件的时候。单线程跑一万遍都是绿的，跑一次四线程就掉 400 条。
//
// 所以这里的断言只有一条主线：**写进去的每一条都必须读得回来**。
// 顺序、延迟、SST 数量都不做要求——那些是实现自由。
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Padded;
using lsm_test::Value;

namespace {

// 故意把 write_buffer 压得很小：800 条数据就能触发好几次 flush，
// 后台 compaction 与前台写入才会真的重叠。buffer 给 4MB 的话，
// 整个测试跑完一次 flush 都不会发生，等于什么都没测。
Options SmallBuffer(const std::string& dir) {
  Options o;
  o.db_path = dir;
  o.write_buffer_size = 64u << 10;
  o.max_file_size = 128u << 10;
  o.l0_compaction_trigger = 2;  // 逼后台赶紧干活
  return o;
}

DB* OpenDb(const Options& o) {
  DB* db = nullptr;
  if (!DB::Open(o, &db).ok()) return nullptr;
  return db;
}

// 每个写线程用不同的 key 前缀，读回来时就能分清是谁的
std::string ThreadKey(int tid, uint64_t i) {
  return "t" + Padded(static_cast<uint64_t>(tid), 2) + "-" + Key(i);
}

std::string ThreadValue(int tid, uint64_t i) {
  return "v" + Padded(static_cast<uint64_t>(tid), 2) + "-" + Value(i);
}

// 起 n 个线程，每个写 [0, per_thread) 条
void ParallelWrite(DB* db, int n, uint64_t per_thread, std::atomic<int>* errors) {
  std::vector<std::thread> ts;
  for (int t = 0; t < n; ++t) {
    ts.emplace_back([db, t, per_thread, errors]() {
      const WriteOptions w;
      for (uint64_t i = 0; i < per_thread; ++i) {
        if (!db->Put(w, Slice(ThreadKey(t, i)), Slice(ThreadValue(t, i))).ok()) {
          errors->fetch_add(1);
        }
      }
    });
  }
  for (auto& th : ts) th.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// 验收测例（labs.md 里 Lab 4 指定的那一条）
// ---------------------------------------------------------------------------
TEST(Concurrency, WritersDoNotLoseKeys) {
  const Options o = SmallBuffer(lsm_test::FreshDir("conc_writers"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  const int kThreads = 4;
  const uint64_t kPerThread = 200;
  std::atomic<int> errors{0};
  ParallelWrite(db, kThreads, kPerThread, &errors);
  EXPECT_EQ(errors.load(), 0);
  ASSERT_TRUE(db->Flush().ok());

  std::string v;
  int missing = 0;
  int wrong = 0;
  for (int t = 0; t < kThreads; ++t) {
    for (uint64_t i = 0; i < kPerThread; ++i) {
      const Status s = db->Get(ReadOptions(), Slice(ThreadKey(t, i)), &v);
      if (!s.ok()) {
        ++missing;
      } else if (v != ThreadValue(t, i)) {
        ++wrong;
      }
    }
  }
  if (missing != 0 || wrong != 0) {
    std::fprintf(stderr, "    missing=%d wrong=%d (总 %d 条)\n", missing, wrong,
                 kThreads * static_cast<int>(kPerThread));
  }
  EXPECT_EQ(missing, 0);
  EXPECT_EQ(wrong, 0);

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 一边写一边 compaction：输入文件被外部删掉的话，这里会读到 ENOENT
// ---------------------------------------------------------------------------
TEST(Concurrency, WritesSurviveConcurrentCompaction) {
  const Options o = SmallBuffer(lsm_test::FreshDir("conc_compact"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  std::atomic<bool> stop{false};
  std::atomic<int> compact_errors{0};

  // 专门有线程不停地催 compaction
  std::thread compactor([db, &stop, &compact_errors]() {
    while (!stop.load()) {
      if (!db->CompactOnce().ok()) compact_errors.fetch_add(1);
      std::this_thread::yield();
    }
  });

  std::atomic<int> errors{0};
  ParallelWrite(db, 3, 150, &errors);

  stop.store(true);
  compactor.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(compact_errors.load(), 0);

  // 写完之后再压一轮，确保最后的读发生在 compaction 之后
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->CompactOnce().ok());

  int missing = 0;
  std::string v;
  for (int t = 0; t < 3; ++t) {
    for (uint64_t i = 0; i < 150; ++i) {
      if (!db->Get(ReadOptions(), Slice(ThreadKey(t, i)), &v).ok()) ++missing;
    }
  }
  if (missing != 0) std::fprintf(stderr, "    compaction 之后丢了 %d 条\n", missing);
  EXPECT_EQ(missing, 0);

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 前台 Flush 与后台刷盘撞车：没 flush_mu_ 会在这里解引用空 imm_
// ---------------------------------------------------------------------------
TEST(Concurrency, ForegroundFlushRacesBackground) {
  const Options o = SmallBuffer(lsm_test::FreshDir("conc_flush"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  std::atomic<bool> stop{false};
  std::thread flusher([db, &stop]() {
    while (!stop.load()) db->Flush();
  });

  std::atomic<int> errors{0};
  ParallelWrite(db, 2, 200, &errors);
  stop.store(true);
  flusher.join();

  EXPECT_EQ(errors.load(), 0);

  int missing = 0;
  std::string v;
  for (int t = 0; t < 2; ++t) {
    for (uint64_t i = 0; i < 200; ++i) {
      if (!db->Get(ReadOptions(), Slice(ThreadKey(t, i)), &v).ok()) ++missing;
    }
  }
  EXPECT_EQ(missing, 0);

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 同一个 key 被一个线程反复覆盖，另一个线程只读：
// 读到的东西可以旧，但**不能倒退**（更老的版本出现在更新的版本之后）
// ---------------------------------------------------------------------------
TEST(Concurrency, ReadersNeverGoBackwards) {
  const Options o = SmallBuffer(lsm_test::FreshDir("conc_monotonic"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  const std::string kKey = "hot";
  ASSERT_TRUE(db->Put(WriteOptions(), Slice(kKey), Slice(Padded(0, 10))).ok());

  const int kRounds = 1500;
  std::atomic<bool> stop{false};
  std::atomic<int> regressions{0};
  std::atomic<int> reads{0};

  std::thread reader([db, &kKey, &stop, &regressions, &reads]() {
    uint64_t last = 0;
    std::string v;
    while (!stop.load()) {
      if (db->Get(ReadOptions(), Slice(kKey), &v).ok()) {
        reads.fetch_add(1);
        const uint64_t seen = std::strtoull(v.c_str(), nullptr, 10);
        if (seen < last) regressions.fetch_add(1);
        last = seen;
      }
    }
  });

  for (int i = 1; i <= kRounds; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Slice(kKey), Slice(Padded(i, 10))).ok());
  }
  stop.store(true);
  reader.join();

  if (regressions.load() != 0) {
    std::fprintf(stderr, "    %d/%d 次读到更老的版本\n", regressions.load(), reads.load());
  }
  EXPECT_EQ(regressions.load(), 0);
  EXPECT_TRUE(reads.load() > 0);

  std::string v;
  ASSERT_TRUE(db->Get(ReadOptions(), Slice(kKey), &v).ok());
  EXPECT_EQ(v, Padded(kRounds, 10));

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---------------------------------------------------------------------------
// 统计口径自洽：每层文件数之和必须等于文件总数
// ---------------------------------------------------------------------------
TEST(Concurrency, StatsLevelFilesAddUp) {
  const Options o = SmallBuffer(lsm_test::FreshDir("conc_stats"));
  DB* db = OpenDb(o);
  ASSERT_TRUE(db != nullptr);

  std::atomic<int> errors{0};
  ParallelWrite(db, 3, 120, &errors);
  ASSERT_TRUE(db->Flush().ok());

  const Stats st = db->GetStats();
  int sum = 0;
  for (int level = 0; level < kNumLevels; ++level) sum += st.level_files[level];

  if (sum != st.num_sst_files) {
    std::fprintf(stderr, "    level_files 之和=%d，num_sst_files=%d\n", sum, st.num_sst_files);
  }
  EXPECT_EQ(sum, st.num_sst_files);
  EXPECT_TRUE(st.flushes > 0);         // 小 buffer + 360 条，至少刷过一次
  EXPECT_TRUE(st.num_sst_files >= 1);  // 刷完至少有文件；压成 1 个也算对

  EXPECT_TRUE(db->Close().ok());
  delete db;
}
