// 参考实现（教师版），仅用于验证骨架。
#include "lsm/dbformat.h"

#include <cstring>

#include "lsm/coding.h"

namespace lsm {
namespace {
inline uint64_t DecodeTag(const Slice& internal_key) {
  return DecodeFixed64(internal_key.data() + internal_key.size() - 8);
}
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint64_t>(t);
}
}  // namespace

Slice AppendInternalKey(std::string* dst, const ParsedInternalKey& k) {
  const size_t old_size = dst->size();
  dst->append(k.user_key.data(), k.user_key.size());
  char buf[8];
  EncodeFixed64(buf, PackSequenceAndType(k.sequence, k.type));
  dst->append(buf, 8);
  return Slice(dst->data() + old_size, dst->size() - old_size);
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  const uint64_t num = DecodeTag(internal_key);
  result->sequence = num >> 8;
  const uint8_t t = static_cast<uint8_t>(num & 0xff);
  if (t > static_cast<uint8_t>(kTypeValue)) return false;
  result->type = static_cast<ValueType>(t);
  result->user_key = Slice(internal_key.data(), n - 8);
  return true;
}

std::string InternalKeyDebugString(const Slice& internal_key) {
  ParsedInternalKey p;
  if (!ParseInternalKey(internal_key, &p)) return "(bad internal key)";
  return p.user_key.ToString() + "@" + std::to_string(p.sequence) + ":" +
         (p.type == kTypeDeletion ? "del" : "val");
}

int InternalKeyComparator::operator()(const Slice& a, const Slice& b) const {
  const Slice ua = ExtractUserKey(a);
  const Slice ub = ExtractUserKey(b);
  const int r = ua.compare(ub);
  if (r != 0) return r;
  // 同一 user key：seq 大的排前面
  const uint64_t na = DecodeTag(a);
  const uint64_t nb = DecodeTag(b);
  if (na > nb) return -1;
  if (na < nb) return 1;
  return 0;
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber sequence) {
  klen_ = static_cast<uint32_t>(user_key.size());
  const size_t total = klen_ + 8;
  char* dst;
  if (total <= sizeof(space_)) {
    dst = space_;
    data_ = space_;
  } else {
    heap_.resize(total);
    dst = &heap_[0];
    data_ = heap_.data();
  }
  std::memcpy(dst, user_key.data(), user_key.size());
  EncodeFixed64(dst + klen_, PackSequenceAndType(sequence, kValueTypeForSeek));
  end_ = data_ + total;
}

}  // namespace lsm
