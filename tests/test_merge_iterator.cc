#include "lsm/merge_iterator.h"

#include <string>
#include <vector>

#include "lsm/dbformat.h"
#include "tests/test_macros.h"

namespace lsm {
namespace {

// 内存里的有序迭代器，用于测试 MergingIterator
class VectorIterator : public Iterator {
 public:
  VectorIterator(std::vector<std::string> keys, std::vector<std::string> values)
      : keys_(std::move(keys)), values_(std::move(values)), idx_(0) {}

  bool Valid() const override { return idx_ < keys_.size(); }
  void SeekToFirst() override { idx_ = 0; }
  void SeekToLast() override { idx_ = keys_.empty() ? 0 : keys_.size() - 1; }
  void Seek(const Slice& target) override {
    idx_ = 0;
    while (idx_ < keys_.size() && Slice(keys_[idx_]).compare(target) < 0) ++idx_;
  }
  void Next() override {
    if (idx_ < keys_.size()) ++idx_;
  }
  void Prev() override {
    if (idx_ > 0) --idx_;
  }
  Slice key() const override { return Slice(keys_[idx_]); }
  Slice value() const override { return Slice(values_[idx_]); }
  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::string> keys_;
  std::vector<std::string> values_;
  size_t idx_;
};

// 辅助：把 user_key + seq 拼成 internal key
std::string MakeInternalKey(const std::string& user_key, SequenceNumber seq,
                            ValueType type = kTypeValue) {
  std::string dst;
  ParsedInternalKey p;
  p.user_key = Slice(user_key);
  p.sequence = seq;
  p.type = type;
  AppendInternalKey(&dst, p);
  return dst;
}

TEST(MergeIterator, OrderAcrossChildren) {
  std::vector<Iterator*> children;
  children.push_back(new VectorIterator({MakeInternalKey("a", 1), MakeInternalKey("c", 1)},
                                        {"va", "vc"}));
  children.push_back(new VectorIterator({MakeInternalKey("b", 1), MakeInternalKey("d", 1)},
                                        {"vb", "vd"}));

  InternalKeyComparator cmp;
  MergingIterator it(&cmp, &children);
  it.SeekToFirst();

  std::vector<std::string> order;
  while (it.Valid()) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(it.key(), &p));
    order.push_back(p.user_key.ToString());
    it.Next();
  }
  ASSERT_TRUE(order.size() == 4u);
  EXPECT_TRUE(order[0] == "a");
  EXPECT_TRUE(order[1] == "b");
  EXPECT_TRUE(order[2] == "c");
  EXPECT_TRUE(order[3] == "d");
}

TEST(MergeIterator, NewerVersionComesFirst) {
  // 同一个 user key 两个版本：seq=5 在第一个孩子，seq=2 在第二个孩子
  std::vector<Iterator*> children;
  children.push_back(new VectorIterator({MakeInternalKey("k", 5)}, {"new"}));
  children.push_back(new VectorIterator({MakeInternalKey("k", 2)}, {"old"}));

  InternalKeyComparator cmp;
  MergingIterator it(&cmp, &children);
  it.SeekToFirst();

  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(it.value() == "new");   // 新的必须排在前面
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_TRUE(it.value() == "old");
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(MergeIterator, SeekLanding) {
  std::vector<Iterator*> children;
  children.push_back(new VectorIterator({MakeInternalKey("a", 1), MakeInternalKey("e", 1)},
                                        {"va", "ve"}));
  children.push_back(new VectorIterator({MakeInternalKey("c", 1)}, {"vc"}));

  InternalKeyComparator cmp;
  MergingIterator it(&cmp, &children);
  it.Seek(Slice(MakeInternalKey("b", 1)));

  ASSERT_TRUE(it.Valid());
  ParsedInternalKey p;
  ASSERT_TRUE(ParseInternalKey(it.key(), &p));
  EXPECT_TRUE(p.user_key.ToString() == "c");
}

TEST(MergeIterator, EmptyChildren) {
  std::vector<Iterator*> children;
  InternalKeyComparator cmp;
  MergingIterator it(&cmp, &children);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
}

}  // namespace
}  // namespace lsm
