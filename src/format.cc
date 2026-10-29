#include "lsm/format.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#include "lsm/coding.h"

namespace lsm {

void BlockHandle::EncodeTo(std::string* dst) const {
  char buf[10];
  dst->append(buf, PutVarint64(buf, offset_));
  dst->append(buf, PutVarint64(buf, size_));
}

Status BlockHandle::DecodeFrom(Slice* input) {
  uint64_t offset = 0;
  uint64_t size = 0;
  if (!GetVarint64(input, &offset) || !GetVarint64(input, &size)) {
    return Status::Corruption("bad block handle");
  }
  offset_ = offset;
  size_ = size;
  return Status::OK();
}

std::string BlockHandle::ToString() const {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "{offset=%llu,size=%llu}",
                static_cast<unsigned long long>(offset_),
                static_cast<unsigned long long>(size_));
  return std::string(buf);
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();

  metaindex_handle.EncodeTo(dst);
  // 补 0 到定长。补 0 而不是补 0xff：这样一次 48 字节读里，
  // 未使用的区域是确定的，调试时一眼能看出「这里没有数据」。
  dst->resize(original_size + kMaxEncodedHandleLength, '\0');

  index_handle.EncodeTo(dst);
  dst->resize(original_size + 2 * kMaxEncodedHandleLength, '\0');

  char buf[8];
  EncodeFixed64(buf, kTableMagicNumber);
  dst->append(buf, 8);

  // 断言式检查：算错了长度，footer 就不再定长，整个读路径的前提就没了
  assert(dst->size() == original_size + kFooterEncodedLength);
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() != kFooterEncodedLength) {
    return Status::Corruption("footer must be 48 bytes");
  }
  const char* start = input->data();

  // magic 固定在最后 8 字节。先校验它，再做任何解析——
  // 否则「读到的是一堆随机字节」会被当成合法的 handle。
  if (DecodeFixed64(start + kFooterEncodedLength - 8) != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic)");
  }

  // 两个 handle 各占定长的 20 字节区域，各自解码时只在自己的那片里走
  Slice meta(start, kMaxEncodedHandleLength);
  Status s = metaindex_handle.DecodeFrom(&meta);
  if (!s.ok()) return s;

  Slice index(start + kMaxEncodedHandleLength, kMaxEncodedHandleLength);
  s = index_handle.DecodeFrom(&index);
  if (!s.ok()) return s;

  input->remove_prefix(kFooterEncodedLength);
  return Status::OK();
}

}  // namespace lsm
