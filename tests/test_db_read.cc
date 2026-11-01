// 阶段 2：读路径。MemTable -> imm_ -> L0 -> L1+，该走的每一层都要走到。
#include <memory>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Value;

namespace {

// 打开一个测试库；目录每次都从空开始
DB* OpenDb(const std::string& dir, const Options& base = Options()) {
  Options o = base;
  o.db_path = dir;
  DB* db = nullptr;
  if (!DB::Open(o, &db).ok()) return nullptr;
  return db;
}

}  // namespace

TEST(DbRead, PutThenGet) {
  const std::string dir = lsm_test::FreshDir("db_putget");
  DB* db = OpenDb(dir);
  ASSERT_TRUE(db != nullptr);

  ASSERT_TRUE(db->Put(WriteOptions(), Slice("k1"), Slice("v1")).ok());
  ASSERT_TRUE(db->Put(WriteOptions(), Slice("k2"), Slice("v2")).ok());

  std::string v;
  ASSERT_TRUE(db->Get(ReadOptions(), Slice("k1"), &v).ok());
  EXPECT_EQ(v, "v1");
  ASSERT_TRUE(db->Get(ReadOptions(), Slice("k2"), &v).ok());
  EXPECT_EQ(v, "v2");

  // 覆盖写：读到的是新值
  ASSERT_TRUE(db->Put(WriteOptions(), Slice("k1"), Slice("v1-new")).ok());
  ASSERT_TRUE(db->Get(ReadOptions(), Slice("k1"), &v).ok());
  EXPECT_EQ(v, "v1-new");

  EXPECT_TRUE(db->Get(ReadOptions(), Slice("nope"), &v).IsNotFound());
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(DbRead, NewestVersionWinsAcrossMemAndSst) {
  const std::string dir = lsm_test::FreshDir("db_layers");
  DB* db = OpenDb(dir);
  ASSERT_TRUE(db != nullptr);

  // 第 1 版进 SST
  ASSERT_TRUE(db->Put(WriteOptions(), Slice(Key(1)), Slice("old")).ok());
  ASSERT_TRUE(db->Flush().ok());

  // 第 2 版还留在 MemTable 里
  ASSERT_TRUE(db->Put(WriteOptions(), Slice(Key(1)), Slice("new")).ok());

  std::string v;
  ASSERT_TRUE(db->Get(ReadOptions(), Slice(Key(1)), &v).ok());
  // MemTable 比 SST 新，必须赢。反过来的症状是「改了没生效」，
  // 而且重启后又变对了 —— 最难查的那类 bug
  EXPECT_EQ(v, "new");

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(DbRead, DeleteHidesOlderValueInSst) {
  const std::string dir = lsm_test::FreshDir("db_delete");
  DB* db = OpenDb(dir);
  ASSERT_TRUE(db != nullptr);

  ASSERT_TRUE(db->Put(WriteOptions(), Slice("k"), Slice("v")).ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->Delete(WriteOptions(), Slice("k")).ok());

  std::string v;
  // 墓碑在 MemTable 里，值在 SST 里：命中墓碑就必须停
  EXPECT_TRUE(db->Get(ReadOptions(), Slice("k"), &v).IsNotFound());

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(DbRead, ManySstsAllReadable) {
  const std::string dir = lsm_test::FreshDir("db_multisst");
  Options o;
  // 很小的 memtable：每 200 条左右就落一个 SST，一定会写出多个文件
  o.write_buffer_size = 8 * 1024;
  o.l0_stop_writes_trigger = 1000;  // 别让写停顿干扰这个用例
  o.l0_compaction_trigger = 4;
  DB* db = OpenDb(dir, o);
  ASSERT_TRUE(db != nullptr);

  const int kCount = 2000;
  WriteOptions w;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(db->Put(w, Slice(Key(i)), Slice(Value(i))).ok());
  }
  ASSERT_TRUE(db->Flush().ok());

  Stats st = db->GetStats();
  EXPECT_TRUE(st.num_sst_files >= 2);  // 确实写了多个 SST
  EXPECT_TRUE(st.flushes >= 2);

  ReadOptions r;
  uint64_t ok = 0;
  for (int i = 0; i < kCount; ++i) {
    std::string v;
    if (db->Get(r, Slice(Key(i)), &v).ok() && v == Value(i)) ++ok;
  }
  EXPECT_EQ(ok, static_cast<uint64_t>(kCount));

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(DbRead, ReopenReadsFromSst) {
  const std::string dir = lsm_test::FreshDir("db_reopen");
  {
    DB* db = OpenDb(dir);
    ASSERT_TRUE(db != nullptr);
    WriteOptions w;
    w.sync = true;
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(w, Slice(Key(i)), Slice(Value(i))).ok());
    }
    ASSERT_TRUE(db->Flush().ok());
    EXPECT_TRUE(db->Close().ok());
    delete db;
  }

  // 第二次打开：MemTable 是空的，所有数据都得从 SST 里读
  DB* db = OpenDb(dir);
  ASSERT_TRUE(db != nullptr);
  const Stats st = db->GetStats();
  EXPECT_TRUE(st.num_sst_files >= 1);

  ReadOptions r;
  for (int i = 0; i < 100; i += 9) {
    std::string v;
    ASSERT_TRUE(db->Get(r, Slice(Key(i)), &v).ok());
    EXPECT_EQ(v, Value(i));
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(DbRead, FilterKeepsMissesCheap) {
  const std::string dir = lsm_test::FreshDir("db_filter");
  Options o;
  o.bits_per_key = 12;  // 1% 假阳性，断言更稳
  DB* db = OpenDb(dir, o);
  ASSERT_TRUE(db != nullptr);

  WriteOptions w;
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put(w, Slice(Key(i)), Slice(Value(i))).ok());
  }
  ASSERT_TRUE(db->Flush().ok());

  const uint64_t before_blocks = db->GetStats().blocks_read;

  // 500 次必然不存在的查询。注意措辞：这些 key 必须**落在 SST 的键范围之内**
  // （key00000000x 排在 key00000000 和 key00000001 之间）。
  // 如果拿 "zzz..." 这种排在所有 key 之后的串去查，索引项 Seek 直接落到
  // 末尾、连 filter 都不用查，那样测的就不是 Bloom 而是索引了。
  ReadOptions r;
  for (int i = 0; i < 500; ++i) {
    std::string v;
    const std::string k = Key(static_cast<uint64_t>(i)) + "x";
    EXPECT_TRUE(db->Get(r, Slice(k), &v).IsNotFound());
  }

  const Stats st = db->GetStats();
  const uint64_t blocks = st.blocks_read - before_blocks;
  // 读放大 = 平均每次未命中读几个块。健康的实现接近 0
  EXPECT_TRUE(blocks <= 50);
  // 每次未命中都该先问一次 Bloom。给一点余量：极少数 key 会刚好落在
  // 索引项的边界上，索引直接给出「这里没有」，不必再问位图。
  EXPECT_TRUE(st.filter_checks >= 450);

  EXPECT_TRUE(db->Close().ok());
  delete db;
}
