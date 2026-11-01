// 阶段 4：引用计数与「删除必须延迟」。
//
// 这门课里只有两处用引用计数：MemTable 和 Version。这两处的错法完全一样——
// 少 Unref 是内存泄漏，多 Unref 是 use-after-free，而 use-after-free 在
// 单线程下几乎测不出来：它只在「别人还在读、你已经 free」的那几微秒里炸。
//
// 所以这组测试不测「能不能跑」，测的是三条必须成立的不变式：
//   1. 最后一个 Unref 之前，对象一定活着（refs 手工 Ref 过的能读到老数据）
//   2. 最后一个 Unref 之后，对象一定被释放（活文件集合里不再有它的文件）
//   3. 活文件集合必须包含「所有还挂在版本链表上的版本」的文件，
//      不能只看 current_——否则正在扫旧版本的读者会遇到 ENOENT
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "lsm/memtable.h"
#include "lsm/table_cache.h"
#include "lsm/thread_safe.h"
#include "lsm/version.h"
#include "lsm/version_edit.h"
#include "lsm/version_set.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::Key;
using lsm_test::Value;

namespace {

// ---------------------------------------------------------------------------
// RefCounted 的行为契约
// ---------------------------------------------------------------------------
class Probe : public RefCounted {
 public:
  static int alive;
  static int cleanups;

  Probe() { ++alive; }
  ~Probe() override { --alive; }

 protected:
  void OnZeroRefs() override { ++cleanups; }
};

int Probe::alive = 0;
int Probe::cleanups = 0;

struct Fixture {
  explicit Fixture(const std::string& tag) : dir(lsm_test::FreshDir(tag)) {
    table_cache.reset(new TableCache(options, dir));
    versions.reset(new VersionSet(options, dir, table_cache.get()));
    recover_status = versions->Recover();
  }

  Status AddSst(int level, uint64_t number, uint64_t lo, uint64_t hi, SequenceNumber seq) {
    lsm_test::SstInfo info =
        lsm_test::WriteSst(options, dir, number, lsm_test::Range(lo, hi), seq);
    if (!info.ok) return Status::IOError("write sst failed");
    VersionEdit edit;
    edit.AddFile(level, info.number, info.file_size, info.smallest, info.largest);
    return versions->LogAndApply(&edit, nullptr);
  }

  // 一条 edit 里「删掉旧文件、加进新文件」——这才是会把旧版本
  // 真正变成孤儿的那种替换。只 AddFile 不改文件集合，旧文件仍然被新版本引用，
  // 测「旧版本释放后文件不再活」就永远测不出来。
  Status Replace(int old_level, uint64_t old_number, int new_level, uint64_t number, uint64_t lo,
                 uint64_t hi, SequenceNumber seq) {
    lsm_test::SstInfo info =
        lsm_test::WriteSst(options, dir, number, lsm_test::Range(lo, hi), seq);
    if (!info.ok) return Status::IOError("write sst failed");
    VersionEdit edit;
    edit.DeleteFile(old_level, old_number);
    edit.AddFile(new_level, info.number, info.file_size, info.smallest, info.largest);
    return versions->LogAndApply(&edit, nullptr);
  }

  bool IsLive(uint64_t number) const {
    std::set<uint64_t> live;
    versions->AddLiveFiles(&live);
    return live.count(number) != 0;
  }

  Options options;
  std::string dir;
  std::unique_ptr<TableCache> table_cache;
  std::unique_ptr<VersionSet> versions;
  Status recover_status;
};

}  // namespace

// ---------------------------------------------------------------------------
// 一、基类的三条契约
// ---------------------------------------------------------------------------
TEST(RefCount, DestroyedExactlyOnLastUnref) {
  Probe::alive = 0;
  Probe::cleanups = 0;

  RefCounted* p = new Probe();
  p->Ref();
  p->Ref();
  p->Ref();
  EXPECT_EQ(Probe::alive, 1);
  EXPECT_EQ(p->refs(), 3);

  p->Unref();
  p->Unref();
  // 还有一次引用没还：对象必须还活着，清理回调也不能提前跑
  EXPECT_EQ(Probe::alive, 1);
  EXPECT_EQ(Probe::cleanups, 0);

  p->Unref();
  EXPECT_EQ(Probe::alive, 0);
  EXPECT_EQ(Probe::cleanups, 1);
}

TEST(RefCount, OnZeroRefsRunsBeforeDestructor) {
  Probe::alive = 0;
  Probe::cleanups = 0;

  RefCounted* p = new Probe();
  p->Ref();
  p->Unref();

  // 先清理、后析构：OnZeroRefs 里如果还要访问成员，此时对象必须完整
  EXPECT_EQ(Probe::cleanups, 1);
  EXPECT_EQ(Probe::alive, 0);
}

// ---------------------------------------------------------------------------
// 二、Version：新版本出现后，旧版本必须挺到最后一个读者放手
// ---------------------------------------------------------------------------
TEST(RefCount, HeldVersionOutlivesReplacement) {
  Fixture f("ref_version_replaced");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(1, 10, 0, 100, 100).ok());

  Version* old = f.versions->current();
  ASSERT_TRUE(old != nullptr);
  old->Ref();  // 模拟「一个读者拿到了旧版本，正在扫表」

  Version* before = f.versions->current();
  ASSERT_TRUE(f.Replace(1, 10, 1, 11, 100, 200, 101).ok());
  Version* now = f.versions->current();
  EXPECT_TRUE(now != before);  // 换版本了

  // 关键：旧版本依然可读。它引用的 000010.sst 不能因为「不是 current_ 了」被删
  EXPECT_EQ(old->NumFiles(1), 1);
  std::string value;
  LookupKey lk(Key(42), kMaxSequenceNumber);
  struct GetStats stats;
  EXPECT_TRUE(old->Get(ReadOptions(), lk, &value, &stats).ok());
  EXPECT_EQ(value, Value(42));

  // 而且它在「活文件」集合里：删除逻辑按这个集合判断，漏一个就是丢数据
  EXPECT_TRUE(f.IsLive(10));

  old->Unref();

  // 放手之后，旧版本被摘链 + 释放，文件 10 不再被任何版本引用
  EXPECT_FALSE(f.IsLive(10));
  EXPECT_TRUE(f.IsLive(11));
}

TEST(RefCount, MultipleRefsNeedMultipleUnrefs) {
  Fixture f("ref_version_multi");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(1, 20, 0, 100, 100).ok());

  Version* v = f.versions->current();
  const int base = v->refs();  // 版本链表自己持有的那一个
  v->Ref();
  v->Ref();
  EXPECT_EQ(v->refs(), base + 2);
  ASSERT_TRUE(f.Replace(1, 20, 1, 21, 100, 200, 101).ok());  // 让它不再是 current_

  v->Unref();
  EXPECT_TRUE(f.IsLive(20));  // 还留着一个引用，文件必须还在
  v->Unref();
  EXPECT_FALSE(f.IsLive(20));
}

TEST(RefCount, LiveFilesCoverEveryHeldVersion) {
  Fixture f("ref_live_set");
  ASSERT_TRUE(f.recover_status.ok());
  ASSERT_TRUE(f.AddSst(1, 30, 0, 100, 100).ok());

  Version* held = f.versions->current();
  held->Ref();
  ASSERT_TRUE(f.Replace(1, 30, 2, 31, 0, 100, 101).ok());
  ASSERT_TRUE(f.AddSst(2, 32, 100, 200, 102).ok());

  // 此刻有两个版本活着：held（只有 30）和 current（31 + 32）。
  // 只按 current_ 算「活文件」的话，30 会被判成垃圾删掉——而可能有人正在扫它。
  EXPECT_TRUE(f.IsLive(30));
  EXPECT_TRUE(f.IsLive(31));
  EXPECT_TRUE(f.IsLive(32));

  held->Unref();
  EXPECT_FALSE(f.IsLive(30));  // 30 只属于被释放的那个旧版本
  EXPECT_TRUE(f.IsLive(31));
  EXPECT_TRUE(f.IsLive(32));
}

// ---------------------------------------------------------------------------
// 三、MemTable：imm_ 被后台线程刷盘时，前台手里那个引用不能断开
// ---------------------------------------------------------------------------
TEST(RefCount, MemTableRefKeepsTableReadable) {
  InternalKeyComparator icmp;
  MemTable* table = new MemTable(icmp);
  table->Ref();  // 模拟「MemTable 落进 imm_，后台开始刷它」
  table->Add(1, kTypeValue, Slice("k"), Slice("v1"));

  // 另一个持有者：正在查这张表的读者
  table->Ref();
  table->Add(2, kTypeValue, Slice("k"), Slice("v2"));
  EXPECT_EQ(table->refs(), 2);

  LookupKey lk("k", 2);
  std::string value;
  Status s;
  ASSERT_TRUE(table->Get(lk, &value, &s));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(value, "v2");

  table->Unref();  // 读者走了
  LookupKey lk1("k", 1);
  std::string v1;
  Status s1;
  ASSERT_TRUE(table->Get(lk1, &v1, &s1));  // 表还是活的
  ASSERT_TRUE(s1.ok());
  EXPECT_EQ(v1, "v1");

  table->Unref();  // 最后一个引用消失，表被 delete
}
