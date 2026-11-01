#include <chrono>
#include <filesystem>
#include <string>

#include "lsm/db.h"
#include "tests/test_macros.h"

namespace lsm {
namespace {

std::string TempDir(const std::string& tag) {
  static int counter = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = std::filesystem::temp_directory_path();
  return (dir / ("lsm_rec_" + tag + "_" + std::to_string(now) + "_" +
                 std::to_string(counter++)))
      .string();
}

TEST(Recovery, ReopenKeepsData) {
  const std::string dir = TempDir("basic");

  {
    Options opt;
    opt.db_path = dir;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opt, &db).ok());
    WriteOptions w;
    w.sync = true;
    for (int i = 0; i < 100; ++i) {
      const std::string k = "key" + std::to_string(i);
      const std::string v = "val" + std::to_string(i);
      ASSERT_TRUE(db->Put(w, Slice(k), Slice(v)).ok());
    }
    delete db;   // 析构里会 Sync WAL
  }

  {
    Options opt;
    opt.db_path = dir;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opt, &db).ok());
    ReadOptions r;
    for (int i = 0; i < 100; ++i) {
      const std::string k = "key" + std::to_string(i);
      std::string v;
      const Status s = db->Get(r, Slice(k), &v);
      ASSERT_TRUE(s.ok());
      EXPECT_TRUE(v == "val" + std::to_string(i));
    }
    delete db;
  }

  std::filesystem::remove_all(dir);
}

TEST(Recovery, DeleteAfterReopen) {
  const std::string dir = TempDir("del");

  {
    Options opt;
    opt.db_path = dir;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opt, &db).ok());
    WriteOptions w;
    ASSERT_TRUE(db->Put(w, Slice("k"), Slice("v")).ok());
    ASSERT_TRUE(db->Delete(w, Slice("k")).ok());
    delete db;
  }

  {
    Options opt;
    opt.db_path = dir;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opt, &db).ok());
    std::string v;
    const Status s = db->Get(ReadOptions(), Slice("k"), &v);
    EXPECT_TRUE(s.IsNotFound());
    delete db;
  }

  std::filesystem::remove_all(dir);
}

TEST(Recovery, MissingKeyNotFound) {
  const std::string dir = TempDir("miss");
  Options opt;
  opt.db_path = dir;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(opt, &db).ok());
  std::string v;
  EXPECT_TRUE(db->Get(ReadOptions(), Slice("nope"), &v).IsNotFound());
  delete db;
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace lsm
