// 参考实现（教师版），仅用于验证骨架。
#include "lsm/write_batch.h"

#include "lsm/coding.h"

namespace lsm {

void WriteBatch::Clear() {
  rep_.clear();
  char buf[12];
  EncodeFixed64(buf, 0);
  EncodeFixed32(buf + 8, 0);
  rep_.append(buf, 12);
}

Status WriteBatch::SetContents(const Slice& data) {
  if (data.size() < 12) return Status::Corruption("WriteBatch contents too short");
  rep_.assign(data.data(), data.size());
  return Status::OK();
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

namespace {
void SetCount(std::string* rep, uint32_t count) {
  char buf[4];
  EncodeFixed32(buf, count);
  rep->replace(8, 4, buf, 4);
}
}  // namespace

void WriteBatch::Put(const Slice& key, const Slice& value) {
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
  SetCount(&rep_, static_cast<uint32_t>(Count() + 1));
}

void WriteBatch::Delete(const Slice& key) {
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
  SetCount(&rep_, static_cast<uint32_t>(Count() + 1));
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < 12) return Status::Corruption("malformed WriteBatch (too small)");
  input.remove_prefix(12);

  int found = 0;
  while (!input.empty()) {
    const char tag = input[0];
    input.remove_prefix(1);
    Slice key;
    Slice value;
    switch (static_cast<uint8_t>(tag)) {
      case kTypeValue:
        if (!GetLengthPrefixedSlice(&input, &key)) return Status::Corruption("bad Put");
        if (!GetLengthPrefixedSlice(&input, &value)) return Status::Corruption("bad Put");
        break;
      case kTypeDeletion:
        if (!GetLengthPrefixedSlice(&input, &key)) return Status::Corruption("bad Delete");
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
    ++found;
    Status s = (static_cast<uint8_t>(tag) == kTypeValue) ? handler->Put(key, value)
                                                        : handler->Delete(key);
    if (!s.ok()) return s;
  }
  if (found != Count()) return Status::Corruption("WriteBatch has wrong count");
  return Status::OK();
}

}  // namespace lsm
