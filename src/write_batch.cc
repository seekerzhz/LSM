#include "lsm/write_batch.h"

#include "lsm/coding.h"

namespace lsm {

void WriteBatch::Clear() {
  rep_.clear();
  char buf[12];
  EncodeFixed64(buf, 0);       // sequence
  EncodeFixed32(buf + 8, 0);   // count
  rep_.append(buf, 12);
}

SequenceNumber WriteBatch::Sequence() const {
  if (rep_.size() < 8) return 0;
  return DecodeFixed64(rep_.data());
}

void WriteBatch::SetSequence(SequenceNumber seq) {
  if (rep_.size() < 8) return;
  char buf[8];
  EncodeFixed64(buf, seq);
  rep_.replace(0, 8, buf, 8);
}

int WriteBatch::Count() const {
  if (rep_.size() < 12) return 0;
  return static_cast<int>(DecodeFixed32(rep_.data() + 8));
}

// 课程提供（完整）：把一个已存在的字节串当作 batch 内容
Status WriteBatch::SetContents(const Slice& data) {
  if (data.size() < 12) return Status::Corruption("WriteBatch contents too short");
  rep_.assign(data.data(), data.size());
  return Status::OK();
}

namespace {
// Put/Delete 里更新头部 count 用。TODO(1.12) 补完实现后这个函数就会被调用。
[[maybe_unused]] void SetCount(std::string* rep, uint32_t count) {
  char buf[4];
  EncodeFixed32(buf, count);
  rep->replace(8, 4, buf, 4);
}
}  // namespace

// ---------------------------------------------------------------------------
// TODO(1.12) Put / Delete / Iterate
//
// Put:   追加 type(1 字节 kTypeValue) + len_prefix(key) + len_prefix(value)，
//        然后 count + 1
// Delete:追加 type(1 字节 kTypeDeletion) + len_prefix(key)，count + 1
// Iterate: 从 offset 12 开始循环 count 次，逐条解析 type / key / value，
//        分发给 handler。最后校验"实际解析条数 == 头部 count"，
//        不等则返回 Corruption（这是检测损坏的第二道防线）。
// ---------------------------------------------------------------------------
void WriteBatch::Put(const Slice& key, const Slice& value) {
  (void)key;
  (void)value;
  // 占位实现：不追加任何内容
}

void WriteBatch::Delete(const Slice& key) {
  (void)key;
  // 占位实现
}

Status WriteBatch::Iterate(Handler* handler) const {
  (void)handler;
  // 占位实现：直接返回 ok，什么都不分发
  return Status::OK();
}

}  // namespace lsm
