// SST 的写入与读取往返：格式对不对、filter 有没有生效、越界怎么处理。
#include <memory>
#include <string>
#include <vector>

#include "lsm/env.h"
#include "lsm/filter_block.h"
#include "lsm/format.h"
#include "lsm/iterator.h"
#include "lsm/table.h"
#include "lsm/table_builder.h"
#include "test_macros.h"
#include "test_util.h"

using namespace lsm;
using lsm_test::IKey;
using lsm_test::Key;

namespace {

// 写一个 SST，返回文件路径。n 条，value 长度固定。
std::string WriteSst(const std::string& dir, const std::string& name, int n,
                     const Options& options) {
  const std::string path = dir + "/" + name;
  std::unique_ptr<WritableFile> file;
  Status s = Env::Default()->NewWritableFile(path, &file);
  if (!s.ok()) return "";

  TableBuilder builder(options, file.get());
  for (int i = 0; i < n; ++i) {
    builder.Add(IKey(Key(static_cast<uint64_t>(i)), 100), lsm_test::Value(static_cast<uint64_t>(i)));
  }
  s = builder.Finish();
  if (!s.ok()) return "";
  file->Sync();
  file.reset();
  return path;
}

struct Opened {
  std::unique_ptr<Table> table;
  Status status;
};

Opened OpenSst(const std::string& path, const Options& options) {
  Opened o;
  uint64_t size = 0;
  Status s = Env::Default()->GetFileSize(path, &size);
  if (!s.ok()) {
    o.status = s;
    return o;
  }
  std::unique_ptr<RandomAccessFile> file;
  s = Env::Default()->NewRandomAccessFile(path, &file);
  if (!s.ok()) {
    o.status = s;
    return o;
  }
  o.status = Table::Open(options, std::move(file), size, &o.table);
  return o;
}

}  // namespace

TEST(Table, RoundTrip1000) {
  Options options;
  options.block_size = 1024;  // 故意小：保证有多于一个块
  const std::string dir = lsm_test::FreshDir("table_roundtrip");
  const std::string path = WriteSst(dir, "t.sst", 1000, options);
  ASSERT_TRUE(!path.empty());

  Opened o = OpenSst(path, options);
  ASSERT_TRUE(o.status.ok());
  ASSERT_TRUE(o.table != nullptr);
  EXPECT_TRUE(o.table->NumDataBlocks() > 1);

  for (int i = 0; i < 1000; i += 37) {
    LookupKey lk(Key(static_cast<uint64_t>(i)), kMaxSequenceNumber);
    std::string value;
    ValueType type = kTypeDeletion;
    struct GetStats stats;
    Status s = o.table->InternalGet(ReadOptions(), lk.internal_key(), &value, &type, &stats);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(type, kTypeValue);
    EXPECT_EQ(value, lsm_test::Value(static_cast<uint64_t>(i)));
  }
}

TEST(Table, MissReturnsNotFound) {
  Options options;
  const std::string dir = lsm_test::FreshDir("table_miss");
  const std::string path = WriteSst(dir, "t.sst", 200, options);
  ASSERT_TRUE(!path.empty());

  Opened o = OpenSst(path, options);
  ASSERT_TRUE(o.status.ok());

  LookupKey lk("no-such-key", kMaxSequenceNumber);
  std::string value;
  ValueType type = kTypeValue;
  struct GetStats stats;
  Status s = o.table->InternalGet(ReadOptions(), lk.internal_key(), &value, &type, &stats);
  EXPECT_TRUE(s.IsNotFound());
}

TEST(Table, GetMissUsesFilter) {
  Options options;
  options.block_size = 512;
  options.bits_per_key = 12;  // 假阳性更低，测试更稳定
  const std::string dir = lsm_test::FreshDir("table_filter");
  const std::string path = WriteSst(dir, "t.sst", 500, options);
  ASSERT_TRUE(!path.empty());

  Opened o = OpenSst(path, options);
  ASSERT_TRUE(o.status.ok());
  ASSERT_TRUE(o.table->has_filter());

  // 查 200 个肯定不存在的 key。绝大多数应该被 filter 直接拒掉，
  // 不产生任何块读取。
  int rejected = 0;
  int blocks_read = 0;
  for (int i = 0; i < 200; ++i) {
    LookupKey lk("absent-" + lsm_test::Padded(static_cast<uint64_t>(i)), kMaxSequenceNumber);
    std::string value;
    ValueType type = kTypeValue;
    struct GetStats stats;
    o.table->InternalGet(ReadOptions(), lk.internal_key(), &value, &type, &stats);
    if (stats.filter_rejected) ++rejected;
    blocks_read += stats.blocks_read;
  }
  EXPECT_TRUE(rejected >= 190);
  EXPECT_TRUE(blocks_read <= 10);
}

TEST(Table, TombstoneReturnsOkWithDeletionType) {
  Options options;
  const std::string dir = lsm_test::FreshDir("table_tombstone");
  const std::string path = dir + "/t.sst";
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(Env::Default()->NewWritableFile(path, &file).ok());
    TableBuilder builder(options, file.get());
    builder.Add(IKey("alive", 100), "v");
    builder.Add(IKey("dead", 100, kTypeDeletion), Slice());
    builder.Add(IKey("empty", 100), Slice());  // 合法空值
    ASSERT_TRUE(builder.Finish().ok());
    file->Sync();
  }

  Opened o = OpenSst(path, options);
  ASSERT_TRUE(o.status.ok());

  {
    LookupKey lk("dead", kMaxSequenceNumber);
    std::string value = "stale";
    ValueType type = kTypeValue;
    struct GetStats stats;
    Status s = o.table->InternalGet(ReadOptions(), lk.internal_key(), &value, &type, &stats);
    // 墓碑必须返回 OK + kTypeDeletion：上层据此停止往下查。
    // 如果返回 NotFound，删除会在更旧的层里「复活」。
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(type, kTypeDeletion);
    EXPECT_TRUE(value.empty());
  }
  {
    LookupKey lk("empty", kMaxSequenceNumber);
    std::string value = "stale";
    ValueType type = kTypeDeletion;
    struct GetStats stats;
    Status s = o.table->InternalGet(ReadOptions(), lk.internal_key(), &value, &type, &stats);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(type, kTypeValue);
    EXPECT_TRUE(value.empty());
  }
}

TEST(Table, IteratorCoversAllEntriesInOrder) {
  Options options;
  options.block_size = 512;
  const std::string dir = lsm_test::FreshDir("table_iter");
  const std::string path = WriteSst(dir, "t.sst", 300, options);
  ASSERT_TRUE(!path.empty());

  Opened o = OpenSst(path, options);
  ASSERT_TRUE(o.status.ok());

  std::unique_ptr<Iterator> it(o.table->NewIterator());
  int count = 0;
  std::string last_user;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string u = lsm_test::UserOf(it->key());
    if (!last_user.empty()) EXPECT_TRUE(last_user < u);
    last_user = u;
    ++count;
  }
  EXPECT_EQ(count, 300);
  EXPECT_TRUE(it->status().ok());
}

TEST(Table, FooterRejectsTruncatedFile) {
  Options options;
  const std::string dir = lsm_test::FreshDir("table_truncated");
  const std::string path = dir + "/t.sst";
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(Env::Default()->NewWritableFile(path, &file).ok());
    file->Append("too short to be a table");
    file->Sync();
  }

  Opened o = OpenSst(path, options);
  EXPECT_TRUE(o.status.IsCorruption());
}

TEST(Table, FooterRejectsBadMagic) {
  Options options;
  const std::string dir = lsm_test::FreshDir("table_badmagic");
  const std::string path = WriteSst(dir, "t.sst", 10, options);
  ASSERT_TRUE(!path.empty());

  // 把最后 8 字节改成垃圾：必须报 Corruption，而不是把随机字节当 footer
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(Env::Default()->NewAppendableFile(path, &file).ok());
    file->Append(Slice(std::string(8, '\x5a')));
    file->Sync();
  }

  // 注意：追加之后 magic 的位置没变（仍在 -8 处），但值变了
  Opened o = OpenSst(path, options);
  EXPECT_TRUE(o.status.IsCorruption());
}

TEST(Table, BlockHandleRoundTrip) {
  BlockHandle a(1234567, 8901);
  std::string enc;
  a.EncodeTo(&enc);

  Slice input(enc);
  BlockHandle b;
  ASSERT_TRUE(b.DecodeFrom(&input).ok());
  EXPECT_EQ(a.offset(), b.offset());
  EXPECT_EQ(a.size(), b.size());
  EXPECT_TRUE(input.empty());
}

TEST(Table, FooterIsExactly48Bytes) {
  Footer f;
  f.metaindex_handle = BlockHandle(100, 20);
  f.index_handle = BlockHandle(120, 30);
  std::string enc;
  f.EncodeTo(&enc);
  EXPECT_EQ(enc.size(), kFooterEncodedLength);

  Slice input(enc);
  Footer g;
  ASSERT_TRUE(g.DecodeFrom(&input).ok());
  EXPECT_EQ(g.metaindex_handle.offset(), 100u);
  EXPECT_EQ(g.index_handle.size(), 30u);
  EXPECT_TRUE(input.empty());
}
