// data block 的编码与查找：重启点、跨重启 Seek、Prev。
#include <memory>
#include <string>

#include "lsm/block.h"
#include "lsm/block_builder.h"
#include "lsm/dbformat.h"
#include "test_macros.h"
#include "key_util.h"

using namespace lsm;
using lsm_test::IKey;
using lsm_test::Key;
using lsm_test::UserOf;

namespace {

std::string BuildBlock(int n, const InternalKeyComparator* cmp_unused = nullptr) {
  (void)cmp_unused;
  BlockBuilder b;
  for (int i = 0; i < n; ++i) {
    b.Add(IKey(Key(static_cast<uint64_t>(i)), 100), "v" + std::to_string(i));
  }
  return b.Finish().ToString();
}

}  // namespace

TEST(Block, RestartPointsEvery16) {
  BlockBuilder b;
  for (int i = 0; i < 40; ++i) {
    b.Add(IKey(Key(static_cast<uint64_t>(i)), 100), "v");
  }
  Block block(b.Finish().ToString());
  ASSERT_TRUE(!block.IsCorrupted());
  EXPECT_EQ(block.CountEntries(), 40u);
  // 0..15 -> 第 16 条开新组；16..31 -> 第 32 条开新组。
  // 于是重启点在 0 / 16 / 32，共 3 个。
  EXPECT_EQ(block.NumRestarts(), 3u);
}

TEST(Block, EmptyBlockIsValidButNoEntries) {
  BlockBuilder b;
  Block block(b.Finish().ToString());
  EXPECT_FALSE(block.IsCorrupted());
  EXPECT_EQ(block.CountEntries(), 0u);
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
}

TEST(Block, IterSeekExactAndBetween) {
  Block block(BuildBlock(40));
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));

  it->Seek(Slice(IKey(Key(17), 100)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(17));

  // 落在 key17 与 key18 之间：应该定位到 key18
  it->Seek(Slice(IKey(Key(17) + "a", 100)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(18));
}

TEST(Block, IterSeekAcrossRestartBoundary) {
  // 32 是一条记录正好在重启点上。从 31 跳到 32 会让迭代器跨组，
  // shared 前缀的语义在这里最容易写错。
  Block block(BuildBlock(40));
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));

  it->Seek(Slice(IKey(Key(32), 100)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(32));

  it->Seek(Slice(IKey(Key(31), 100)));
  ASSERT_TRUE(it->Valid());
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(32));
}

TEST(Block, IterSeekBeforeFirstAndAfterLast) {
  Block block(BuildBlock(20));
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));

  it->Seek(Slice(IKey("aaa", 100)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(0));

  it->Seek(Slice(IKey("zzzz", 100)));
  EXPECT_FALSE(it->Valid());
}

TEST(Block, IterPrevWalksBackwards) {
  Block block(BuildBlock(40));
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));

  it->SeekToFirst();
  for (int i = 0; i < 20; ++i) it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(20));

  // 往回走 5 步，跨过一次重启点边界（20 -> 16 会跨组）
  for (int i = 0; i < 5; ++i) it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(15));

  // 再走到头
  for (int i = 0; i < 15; ++i) it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(0));
  it->Prev();
  EXPECT_FALSE(it->Valid());
}

TEST(Block, IterSeekToLast) {
  Block block(BuildBlock(40));
  InternalKeyComparator cmp;
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(UserOf(it->key()), Key(39));
}

TEST(Block, SameUserKeyKeepsSequenceDescending) {
  // 同一个 user_key 的三个版本，seq 必须从大到小排列：
  // 这样 Seek 找到的第一个就是最新版本。
  //
  // 注意 builder 必须拿到比较器：这三个 key 的**字节序**是 seq 300 < 200 < 100
  // 的反面（小端编码），用 memcmp 判序会直接断言失败。这正是内部键的经典坑。
  InternalKeyComparator cmp;
  BlockBuilder b(&cmp);
  b.Add(IKey("dup", 300), "v300");
  b.Add(IKey("dup", 200), "v200");
  b.Add(IKey("dup", 100), "v100");

  Block block(b.Finish().ToString());
  std::unique_ptr<Iterator> it(block.NewIterator(&cmp));

  it->Seek(Slice(IKey("dup", kMaxSequenceNumber)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value().ToString(), "v300");

  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value().ToString(), "v200");
}

TEST(Block, CorruptRestartCountIsDetected) {
  // 把块尾的 restart 个数改成一个荒谬的值：必须被识别为损坏，
  // 而不是让后面的指针运算越界。
  std::string raw = BuildBlock(10);
  char* p = &raw[raw.size() - 4];
  p[0] = static_cast<char>(0xff);
  p[1] = static_cast<char>(0xff);
  p[2] = static_cast<char>(0xff);
  p[3] = static_cast<char>(0x7f);

  Block block(raw);
  EXPECT_TRUE(block.IsCorrupted());
}
