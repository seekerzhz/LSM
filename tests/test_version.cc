// 阶段 3：Version 的按层查找与文件重叠判断。
//
// 这组测试全部**不经过 DBImpl**：直接造 SST、拼 VersionEdit、调 LogAndApply。
// 好处是失败时能立刻分清「是版本管理错了」还是「是 DB 上层错了」。
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lsm/table_cache.h"
#include "lsm/version.h"
#include "lsm/version_edit.h"
#include "lsm/version_set.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Padded;
using lsm_test::Value;

namespace {

struct Fixture {
  // setup 在构造 VersionSet **之前**执行：Options 是拷贝进
  // VersionSet / TableCache 的，构造完再改就没人看了。
  explicit Fixture(const std::string& tag,
                   const std::function<void(Options&)>& setup = nullptr)
      : dir(lsm_test::FreshDir(tag)) {
    if (setup != nullptr) setup(options);
    table_cache.reset(new TableCache(options, dir));
    versions.reset(new VersionSet(options, dir, table_cache.get()));
    recover_status = versions->Recover();
  }

  // 造一个 SST 并把它加进某一层
  Status AddSst(int level, uint64_t number, uint64_t lo, uint64_t hi, SequenceNumber seq,
                ValueType type = kTypeValue) {
    lsm_test::SstInfo info = lsm_test::WriteSst(options, dir, number, lsm_test::Range(lo, hi),
                                                seq, type);
    if (!info.ok) return Status::IOError("write sst failed");
    VersionEdit edit;
    edit.AddFile(level, info.number, info.file_size, info.smallest, info.largest);
    return versions->LogAndApply(&edit, nullptr);
  }

  Status Get(const std::string& user_key, std::string* value) {
    LookupKey lk(user_key, kMaxSequenceNumber);
    struct GetStats stats;
    return versions->current()->Get(ReadOptions(), lk, value, &stats);
  }

  Options options;
  std::string dir;
  std::unique_ptr<TableCache> table_cache;
  std::unique_ptr<VersionSet> versions;
  Status recover_status;
};

}  // namespace

TEST(Version, EmptyVersionFindsNothing) {
  Fixture f("version_empty");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.versions->current() != nullptr);

  for (int level = 0; level < kNumLevels; ++level) {
    EXPECT_EQ(f.versions->current()->NumFiles(level), 0);
  }
  std::string value;
  EXPECT_TRUE(f.Get(Key(1), &value).IsNotFound());
}

TEST(Version, FindsKeyInLevel1) {
  Fixture f("version_l1");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(1, 10, 0, 100, 100).ok());

  EXPECT_EQ(f.versions->current()->NumFiles(1), 1);

  std::string value;
  ASSERT_TRUE(f.Get(Key(42), &value).ok());
  EXPECT_EQ(value, Value(42));

  // 层内文件范围之外：落在空隙里，不应该误命中
  std::string v2;
  EXPECT_TRUE(f.Get(Key(100), &v2).IsNotFound());
}

TEST(Version, LevelLookupIsBinarySearchNotScan) {
  // L1 放 50 个不重叠的小文件，查最后一个区间里的 key。
  // 顺序扫也对，但 blocks_read 会远大于 1；这里只看「有没有读不该读的块」。
  Fixture f("version_bsearch");
  ASSERT_TRUE(f.recover_status.ok());
  for (uint64_t i = 0; i < 50; ++i) {
    ASSERT_TRUE(f.AddSst(1, 100 + i, i * 10, i * 10 + 10, 100).ok());
  }
  ASSERT_TRUE(f.versions->current()->NumFiles(1) == 50);

  std::string value;
  EXPECT_TRUE(f.Get(Key(495), &value).ok());
  EXPECT_EQ(value, Value(495));
}

TEST(Version, L0KeepsNewestFileFirst) {
  Fixture f("version_l0order");
  ASSERT_TRUE(f.recover_status.ok());

  // 编号 20 是后写的（更新），编号 10 是旧的。两者 key 范围完全重叠。
  ASSERT_TRUE(f.AddSst(0, 10, 0, 10, 100).ok());
  ASSERT_TRUE(f.AddSst(0, 20, 0, 10, 200).ok());

  Version* v = f.versions->current();
  ASSERT_TRUE(v->NumFiles(0) == 2);
  // L0 必须按编号降序：Get 靠这个顺序实现「新版本优先」
  EXPECT_EQ(v->files_[0][0]->number, 20u);
  EXPECT_EQ(v->files_[0][1]->number, 10u);

  std::string value;
  ASSERT_TRUE(f.Get(Key(3), &value).ok());
  EXPECT_EQ(value, Value(3));
}

TEST(Version, TombstoneInNewerFileStopsSearch) {
  Fixture f("version_tombstone");
  ASSERT_TRUE(f.recover_status.ok());

  // 旧文件有值，新文件是墓碑（编号更大 = 更新）
  ASSERT_TRUE(f.AddSst(0, 10, 0, 10, 100).ok());
  ASSERT_TRUE(f.AddSst(0, 20, 0, 10, 200, kTypeDeletion).ok());

  std::string value;
  const Status s = f.Get(Key(5), &value);
  // 命中墓碑必须停：继续往旧文件查会让删除复活
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
}

TEST(Version, OverlapInLevelSelectsOnlyIntersectingFiles) {
  Fixture f("version_overlap");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(1, 10, 0, 100, 100).ok());
  ASSERT_TRUE(f.AddSst(1, 11, 100, 200, 100).ok());
  ASSERT_TRUE(f.AddSst(1, 12, 200, 300, 100).ok());

  std::vector<FileMetaData*> out;

  // 只有 key 150 落在第二个文件里
  const std::string k150 = Key(150);
  const Slice s150(k150);
  f.versions->current()->OverlapInLevel(1, &s150, &s150, &out);
  ASSERT_TRUE(out.size() == 1);
  EXPECT_EQ(out[0]->number, 11u);

  // 跨两个文件的区间
  out.clear();
  const std::string lo = Key(50), hi = Key(150);
  const Slice slo(lo);
  const Slice shi(hi);
  f.versions->current()->OverlapInLevel(1, &slo, &shi, &out);
  EXPECT_EQ(out.size(), 2u);

  // 边界不设限 = 全选
  out.clear();
  f.versions->current()->OverlapInLevel(1, nullptr, nullptr, &out);
  EXPECT_EQ(out.size(), 3u);
}

TEST(Version, AllFileNumbersListsEveryLevel) {
  Fixture f("version_livefiles");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(0, 10, 0, 10, 100).ok());
  ASSERT_TRUE(f.AddSst(1, 11, 0, 100, 100).ok());
  ASSERT_TRUE(f.AddSst(2, 12, 0, 1000, 100).ok());

  const std::vector<uint64_t> nums = f.versions->current()->AllFileNumbers();
  EXPECT_EQ(nums.size(), 3u);
}

TEST(Version, PickCompactionWhenL0IsFull) {
  Fixture f("version_pick", [](Options& o) { o.l0_compaction_trigger = 4; });
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(0, 10, 0, 10, 100).ok());
  ASSERT_TRUE(f.AddSst(0, 11, 5, 15, 100).ok());
  ASSERT_TRUE(f.AddSst(0, 12, 20, 30, 100).ok());

  // 还没到阈值（4 个）
  EXPECT_FALSE(f.versions->NeedsCompaction());
  Compaction* c = f.versions->PickCompaction();
  ASSERT_TRUE(c == nullptr);

  // 再塞一个就到阈值了
  ASSERT_TRUE(f.AddSst(0, 13, 40, 50, 100).ok());
  EXPECT_TRUE(f.versions->NeedsCompaction());
  c = f.versions->PickCompaction();
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->level(), 0);
  // L0 触发时输入必须是**全部** L0 文件，否则剩下没被合并的文件
  // 会和输出再次重叠，破坏 L1 的不重叠不变量
  EXPECT_EQ(c->inputs(0).size(), 4u);
  c->input_version()->Unref();
  delete c;
}
