// 阶段 3：compaction 的三条丢弃规则 + 输出文件的切分约束。
//
// 这组测试走 DB 的公开接口，只靠 Stats 和「读回来对不对」下判断，
// 不碰任何内部结构 —— 因为学生实现内部怎么切文件是他的自由，
// 但「哪些记录可以丢」和「切完还能不能读对」是不能变的。
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "lsm/env.h"
#include "lsm/table.h"
#include "lsm/table_cache.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Padded;
using lsm_test::Value;

namespace {

// 写 n 条 [lo, hi) 的 key 然后 flush 成一个 L0 文件
void WriteAndFlush(DB* db, uint64_t lo, uint64_t hi, const std::string& value_tag = "") {
  WriteOptions w;
  for (uint64_t i = lo; i < hi; ++i) {
    const std::string v = value_tag.empty() ? Value(i) : value_tag;
    const Status s = db->Put(w, Slice(Key(i)), Slice(v));
    if (!s.ok()) {
      std::fprintf(stderr, "    put %s failed: %s\n", Key(i).c_str(), s.ToString().c_str());
      return;
    }
  }
  const Status fs = db->Flush();
  if (!fs.ok()) std::fprintf(stderr, "    flush failed: %s\n", fs.ToString().c_str());
}

// 目录里所有 .sst 的文件名（升序）
std::vector<std::string> SstFiles(const std::string& dir) {
  std::vector<std::string> names;
  std::vector<std::string> all;
  if (!Env::Default()->ListDir(dir, &all).ok()) return names;
  for (const std::string& n : all) {
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".sst") == 0) names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  return names;
}

struct FileKeys {
  std::string name;
  std::vector<std::string> user_keys;
};

// 打开一个 SST，按顺序收集它的 user_key
FileKeys KeysInFile(const std::string& dir, const std::string& name, const Options& options) {
  FileKeys out;
  out.name = name;
  const std::string path = dir + "/" + name;
  uint64_t size = 0;
  if (!Env::Default()->GetFileSize(path, &size).ok()) return out;

  std::unique_ptr<RandomAccessFile> file;
  if (!Env::Default()->NewRandomAccessFile(path, &file).ok()) return out;
  std::unique_ptr<Table> table;
  if (!Table::Open(options, std::move(file), size, &table).ok()) return out;

  std::unique_ptr<Iterator> it(table->NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.user_keys.push_back(lsm_test::UserOf(it->key()));
  }
  return out;
}

}  // namespace

TEST(Compaction, L0FilesMergeIntoBaseLevel) {
  const std::string dir = lsm_test::FreshDir("compact_l0");
  Options o;
  o.db_path = dir;
  o.l0_compaction_trigger = 4;
  o.l0_stop_writes_trigger = 64;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, &db).ok());

  // 四轮 flush -> 四个 L0 文件
  for (uint64_t r = 0; r < 4; ++r) WriteAndFlush(db, r * 100, r * 100 + 100);

  Stats before = db->GetStats();
  ASSERT_TRUE(before.level_files[0] >= 1);  // 至少还能看到 L0（也可能已被后台压掉）

  db->CompactOnce();
  // 后台线程可能也在压，所以这里等到「L0 空 + L1 有文件」为止
  for (int i = 0; i < 200; ++i) {
    Stats s = db->GetStats();
    if (s.level_files[0] == 0 && s.level_files[1] > 0) break;
    db->CompactOnce();
  }

  Stats after = db->GetStats();
  EXPECT_EQ(after.level_files[0], 0);        // L0 被压空了
  EXPECT_TRUE(after.level_files[1] >= 1);    // 输出进了 L1
  EXPECT_TRUE(after.compactions >= 1);

  // 数据一条都不能少
  ReadOptions r;
  for (uint64_t i = 0; i < 400; i += 7) {
    std::string v;
    ASSERT_TRUE(db->Get(r, Slice(Key(i)), &v).ok());
    EXPECT_EQ(v, Value(i));
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(Compaction, DropsOverwrittenVersions) {
  const std::string dir = lsm_test::FreshDir("compact_drop");
  Options o;
  o.db_path = dir;
  o.l0_compaction_trigger = 4;
  o.l0_stop_writes_trigger = 64;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, &db).ok());

  // 同一批 key 写 4 遍，每遍一个 SST：磁盘上有 4 份，只有最后一份有用
  const std::string tag[4] = {"v0", "v1", "v2", "v3"};
  for (int round = 0; round < 4; ++round) {
    WriteAndFlush(db, 0, 100, tag[round] + std::string(60, 'x'));
  }

  const uint64_t before_bytes = db->GetStats().disk_usage;
  for (int i = 0; i < 200; ++i) {
    db->CompactOnce();
    Stats s = db->GetStats();
    if (s.level_files[0] == 0 && s.level_files[1] > 0) break;
  }

  Stats after = db->GetStats();
  EXPECT_TRUE(after.compactions >= 1);
  // 被覆盖的旧版本必须丢掉：压缩后体积应该明显变小
  EXPECT_TRUE(after.disk_usage < before_bytes / 2);

  ReadOptions r;
  for (uint64_t i = 0; i < 100; i += 3) {
    std::string v;
    ASSERT_TRUE(db->Get(r, Slice(Key(i)), &v).ok());
    EXPECT_EQ(v, tag[3] + std::string(60, 'x'));
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(Compaction, TombstoneDroppedAtBaseLevel) {
  const std::string dir = lsm_test::FreshDir("compact_tombstone");
  Options o;
  o.db_path = dir;
  o.l0_compaction_trigger = 4;
  o.l0_stop_writes_trigger = 64;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, &db).ok());

  WriteAndFlush(db, 0, 50);          // L0#1：数据
  WriteAndFlush(db, 50, 100);        // L0#2：凑数，保证 L0 够 4 个
  WriteOptions w;
  for (uint64_t i = 0; i < 50; ++i) db->Delete(w, Slice(Key(i)));  // 墓碑
  db->Flush();                       // L0#3
  WriteAndFlush(db, 100, 150);       // L0#4

  for (int i = 0; i < 200; ++i) {
    db->CompactOnce();
    Stats s = db->GetStats();
    if (s.level_files[0] == 0 && s.compactions >= 1) break;
  }

  ReadOptions r;
  // 墓碑和它盖住的数据都不该再存在
  for (uint64_t i = 0; i < 50; i += 5) {
    std::string v;
    EXPECT_TRUE(db->Get(r, Slice(Key(i)), &v).IsNotFound());
  }
  // 没被删的照旧
  for (uint64_t i = 50; i < 150; i += 9) {
    std::string v;
    ASSERT_TRUE(db->Get(r, Slice(Key(i)), &v).ok());
    EXPECT_EQ(v, Value(i));
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(Compaction, KeepsEveryLiveKey) {
  const std::string dir = lsm_test::FreshDir("compact_keep");
  Options o;
  o.db_path = dir;
  o.l0_compaction_trigger = 4;
  o.l0_stop_writes_trigger = 64;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, &db).ok());
  for (uint64_t r = 0; r < 8; ++r) WriteAndFlush(db, r * 200, r * 200 + 200, "");

  for (int i = 0; i < 200; ++i) {
    db->CompactOnce();
    Stats s = db->GetStats();
    if (s.level_files[0] == 0 && s.level_files[1] > 0) break;
  }

  ReadOptions r;
  uint64_t found = 0;
  int shown = 0;
  for (uint64_t i = 0; i < 1600; ++i) {
    std::string v;
    const Status s = db->Get(r, Slice(Key(i)), &v);
    if (s.ok() && v == Value(i)) {
      ++found;
    } else if (shown < 5) {
      ++shown;
      std::fprintf(stderr, "    bad %s status=%s value=%.24s\n", Key(i).c_str(),
                   s.ToString().c_str(), v.c_str());
    }
  }
  EXPECT_EQ(found, 1600u);
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

TEST(Compaction, OutputFilesDoNotShareUserKeys) {
  // 把 max_file_size 调到很小，强制一轮 compaction 产生多个输出文件。
  // 切点只能在「换了一个 user_key」的位置：否则同一个 key 的多个版本
  // 会被分到两个文件，第二个文件的 smallest 就无法代表「这个 key 从这里开始」，
  // 之后按层二分查找会漏掉它。
  const std::string dir = lsm_test::FreshDir("compact_split");
  Options o;
  o.db_path = dir;
  o.l0_compaction_trigger = 4;
  o.l0_stop_writes_trigger = 64;
  o.max_file_size = 2048;  // 每个输出文件约 2KB
  o.block_size = 512;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, &db).ok());
  for (uint64_t r = 0; r < 4; ++r) WriteAndFlush(db, r * 150, r * 150 + 150);

  for (int i = 0; i < 200; ++i) {
    db->CompactOnce();
    Stats s = db->GetStats();
    if (s.level_files[0] == 0 && s.level_files[1] > 0) break;
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;

  const std::vector<std::string> files = SstFiles(dir);
  ASSERT_TRUE(files.size() >= 2);  // 确实切了多个文件，否则这个用例没意义

  std::set<std::string> seen;
  for (const std::string& name : files) {
    const FileKeys fk = KeysInFile(dir, name, o);
    ASSERT_TRUE(!fk.user_keys.empty());
    for (const std::string& k : fk.user_keys) {
      // 同一个 user_key 出现在两个文件里 = 切点落在了 key 中间
      EXPECT_TRUE(seen.insert(k).second);
    }
  }
  EXPECT_EQ(seen.size(), 600u);
}
