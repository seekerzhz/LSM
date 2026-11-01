// 参考实现（教师版），仅用于验证骨架的正确性。不随站点发布。
#include "lsm/coding.h"

#include <cstring>

namespace lsm {

int PutVarint32(char* dst, uint32_t v) {
  int n = 0;
  while (v >= 128) {
    dst[n++] = static_cast<char>((v & 0x7f) | 0x80);
    v >>= 7;
  }
  dst[n++] = static_cast<char>(v);
  return n;
}

int PutVarint64(char* dst, uint64_t v) {
  int n = 0;
  while (v >= 128) {
    dst[n++] = static_cast<char>((v & 0x7f) | 0x80);
    v >>= 7;
  }
  dst[n++] = static_cast<char>(v);
  return n;
}

bool GetVarint32(Slice* input, uint32_t* v) {
  const char* p = input->data();
  const size_t n = input->size();
  uint32_t result = 0;
  for (int i = 0, shift = 0; i < 5; ++i, shift += 7) {
    if (static_cast<size_t>(i) >= n) return false;
    const uint32_t byte = static_cast<unsigned char>(p[i]);
    result |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      input->remove_prefix(static_cast<size_t>(i) + 1);
      return true;
    }
  }
  return false;
}

bool GetVarint64(Slice* input, uint64_t* v) {
  const char* p = input->data();
  const size_t n = input->size();
  uint64_t result = 0;
  for (int i = 0, shift = 0; i < 10; ++i, shift += 7) {
    if (static_cast<size_t>(i) >= n) return false;
    const uint64_t byte = static_cast<unsigned char>(p[i]);
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      input->remove_prefix(static_cast<size_t>(i) + 1);
      return true;
    }
  }
  return false;
}

void EncodeFixed32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xff);
  dst[1] = static_cast<char>((v >> 8) & 0xff);
  dst[2] = static_cast<char>((v >> 16) & 0xff);
  dst[3] = static_cast<char>((v >> 24) & 0xff);
}

void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

uint32_t DecodeFixed32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0]))) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

uint64_t DecodeFixed64(const char* p) {
  return static_cast<uint64_t>(DecodeFixed32(p)) |
         (static_cast<uint64_t>(DecodeFixed32(p + 4)) << 32);
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& v) {
  char buf[10];
  const int n = PutVarint32(buf, static_cast<uint32_t>(v.size()));
  dst->append(buf, static_cast<size_t>(n));
  dst->append(v.data(), v.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) return false;
  if (input->size() < len) return false;
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace lsm
