// 阶段 3：MANIFEST 的重放、CURRENT 的指向与损坏检测。
//
// MANIFEST 是「增量日志」，CURRENT 是指向最新一份 MANIFEST 的那一行。
// 这组测试要覆盖三件事：
//   1. 很多条 edit 逐条重放后，文件集合与写之前完全一致
//   2. CURRENT 坏了/指丢了，必须报错而不是当成空库（当成空库 = 静默丢数据）
//   3. 每次启动会重写成一条快照，MANIFEST 不会无限变长
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

namespace {

// 读一个小文本文件（CURRENT 只有几十字节）
std::string ReadWholeFile(const std::string& path) {
  std::unique_ptr<SequentialFile> file;
  if (!Env::Default()->NewSequentialFile(path, &file).ok()) return std::string();
  std::string out;
  char buf[512];
  Slice piece;
  while (true) {
    if (!file->Read(sizeof(buf), &piece, buf).ok()) break;
    if (piece.empty()) break;
    out.append(piece.data(), piece.size());
  }
  return out;
}

}  // namespace

TEST(Manifest, ReplaysFiftyEdits) {
  const std::string dir = lsm_test::FreshDir("manifest_replay");
  Options options;
  std::vector<uint64_t> numbers;

  {
    TableCache tc(options, dir);
    VersionSet vs(options, dir, &tc);
    ASSERT_TRUE(vs.Recover().ok());

    // 50 个互不重叠的文件，一个 edit 加一个 —— MANIFEST 会变成 50 条记录
    for (uint64_t i = 0; i < 50; ++i) {
      const uint64_t number = 100 + i;
      lsm_test::SstInfo info =
          lsm_test::WriteSst(options, dir, number, lsm_test::Range(i * 10, i * 10 + 10), 100);
      ASSERT_TRUE(info.ok);

      VersionEdit edit;
      edit.AddFile(1, info.number, info.file_size, info.smallest, info.largest);
      edit.SetLastSequence(100 + i);
      ASSERT_TRUE(vs.LogAndApply(&edit, nullptr).ok());
      numbers.push_back(number);
    }
    EXPECT_EQ(vs.current()->NumFiles(1), 50);
  }

  // 重新打开：这次必须靠「重放 50 条 edit」才能重建出同样的文件集合
  TableCache tc2(options, dir);
  VersionSet vs2(options, dir, &tc2);
  ASSERT_TRUE(vs2.Recover().ok());
  EXPECT_EQ(vs2.current()->NumFiles(1), 50);
  EXPECT_EQ(vs2.LastSequence(), 149u);

  std::vector<uint64_t> got = vs2.current()->AllFileNumbers();
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got.size(), numbers.size());
  for (size_t i = 0; i < numbers.size() && i < got.size(); ++i) {
    EXPECT_EQ(got[i], numbers[i]);
  }
}

TEST(Manifest, MissingSstIsCorruption) {
  const std::string dir = lsm_test::FreshDir("manifest_missing");
  Options options;
  uint64_t victim = 0;

  {
    TableCache tc(options, dir);
    VersionSet vs(options, dir, &tc);
    ASSERT_TRUE(vs.Recover().ok());
    for (uint64_t i = 0; i < 3; ++i) {
      lsm_test::SstInfo info =
          lsm_test::WriteSst(options, dir, 200 + i, lsm_test::Range(i * 10, i * 10 + 10), 100);
      ASSERT_TRUE(info.ok);
      VersionEdit edit;
      edit.AddFile(0, info.number, info.file_size, info.smallest, info.largest);
      ASSERT_TRUE(vs.LogAndApply(&edit, nullptr).ok());
    }
    victim = 201;
  }

  ASSERT_TRUE(Env::Default()->RemoveFile(TableCache::FileName(dir, victim)).ok());

  TableCache tc2(options, dir);
  VersionSet vs2(options, dir, &tc2);
  const Status s = vs2.Recover();
  // 不许「自动删掉元数据项来修复」——那等于静默丢数据
  EXPECT_TRUE(s.IsCorruption());
}

TEST(Manifest, CurrentPointsToLatestManifest) {
  const std::string dir = lsm_test::FreshDir("manifest_current");
  Options options;

  TableCache tc(options, dir);
  VersionSet vs(options, dir, &tc);
  ASSERT_TRUE(vs.Recover().ok());

  const std::string current = ReadWholeFile(VersionSet::CurrentFileName(dir));
  ASSERT_TRUE(!current.empty());
  std::string trimmed = current;
  while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) trimmed.pop_back();

  char expect[64];
  std::snprintf(expect, sizeof(expect), "MANIFEST-%06llu",
                static_cast<unsigned long long>(vs.manifest_file_number()));
  EXPECT_EQ(trimmed, std::string(expect));
  EXPECT_TRUE(Env::Default()->FileExists(dir + "/" + trimmed));
}

TEST(Manifest, GarbageCurrentIsRejected) {
  const std::string dir = lsm_test::FreshDir("manifest_badcurrent");
  Options options;

  {
    TableCache tc(options, dir);
    VersionSet vs(options, dir, &tc);
    ASSERT_TRUE(vs.Recover().ok());
  }

  // 写一句不可能是 MANIFEST 名字的内容
  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(Env::Default()->NewWritableFile(VersionSet::CurrentFileName(dir), &f).ok());
    f->Append("this-is-not-a-manifest-name\n");
    ASSERT_TRUE(f->Sync().ok());
  }

  TableCache tc2(options, dir);
  VersionSet vs2(options, dir, &tc2);
  const Status s = vs2.Recover();
  // 关键：**不能**当成「全新库」去建空版本，否则用户的数据就消失了
  EXPECT_TRUE(s.IsCorruption());
}

TEST(Manifest, ReopenRewritesSnapshot) {
  const std::string dir = lsm_test::FreshDir("manifest_snapshot");
  Options options;
  uint64_t first_manifest = 0;
  uint64_t log_number = 0;

  {
    TableCache tc(options, dir);
    VersionSet vs(options, dir, &tc);
    ASSERT_TRUE(vs.Recover().ok());

    lsm_test::SstInfo info = lsm_test::WriteSst(options, dir, 300, lsm_test::Range(0, 10), 50);
    ASSERT_TRUE(info.ok);
    VersionEdit edit;
    edit.AddFile(0, info.number, info.file_size, info.smallest, info.largest);
    edit.SetLogNumber(7);
    ASSERT_TRUE(vs.LogAndApply(&edit, nullptr).ok());

    first_manifest = vs.manifest_file_number();
    log_number = vs.log_number();
  }

  TableCache tc2(options, dir);
  VersionSet vs2(options, dir, &tc2);
  ASSERT_TRUE(vs2.Recover().ok());
  // 重写快照：新 MANIFEST 的编号必须更大，内容却只有一条记录
  EXPECT_TRUE(vs2.manifest_file_number() > first_manifest);
  EXPECT_EQ(vs2.current()->NumFiles(0), 1);
  // log_number 是从 edit 里带过来的，不能被重写丢掉
  EXPECT_EQ(vs2.log_number(), log_number);
}

TEST(Manifest, DeletedFileLeavesVersion) {
  const std::string dir = lsm_test::FreshDir("manifest_delete");
  Options options;

  TableCache tc(options, dir);
  VersionSet vs(options, dir, &tc);
  ASSERT_TRUE(vs.Recover().ok());

  lsm_test::SstInfo a = lsm_test::WriteSst(options, dir, 400, lsm_test::Range(0, 10), 100);
  lsm_test::SstInfo b = lsm_test::WriteSst(options, dir, 401, lsm_test::Range(10, 20), 100);
  ASSERT_TRUE(a.ok && b.ok);

  {
    VersionEdit edit;
    edit.AddFile(1, a.number, a.file_size, a.smallest, a.largest);
    edit.AddFile(1, b.number, b.file_size, b.smallest, b.largest);
    ASSERT_TRUE(vs.LogAndApply(&edit, nullptr).ok());
  }
  ASSERT_TRUE(vs.current()->NumFiles(1) == 2);

  // 让一个「读者」持有当前版本，之后换版本它还能看到旧文件
  Version* reader = vs.current();
  reader->Ref();

  {
    VersionEdit edit;
    edit.DeleteFile(1, a.number);
    ASSERT_TRUE(vs.LogAndApply(&edit, nullptr).ok());
  }
  EXPECT_EQ(vs.current()->NumFiles(1), 1);
  // 旧版本还在，所以它引用的文件不能被删
  EXPECT_TRUE(Env::Default()->FileExists(TableCache::FileName(dir, a.number)));
  EXPECT_EQ(reader->NumFiles(1), 2);

  reader->Unref();
  // 最后一个读者走了，延迟删除才生效
  EXPECT_FALSE(Env::Default()->FileExists(TableCache::FileName(dir, a.number)));
}
