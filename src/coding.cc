#include "lsm/coding.h"

#include <cstring>

namespace lsm {

// ---------------------------------------------------------------------------
// TODO(0.1) PutVarint32 / PutVarint64
//
// 输入：dst 至少有 5（或 10）字节可写；v 为任意值。
// 输出：返回写入的字节数；dst[0..n) 是编码结果。
// 约束：
//   - v == 0 必须编码成单字节 0x00（注意用 do-while，普通 while 会写出 0 字节）
//   - 每字节低 7 位存数据，最高位表示"还有后续"
//   - UINT32_MAX 编码为 ff ff ff ff 0f（5 字节）
// ---------------------------------------------------------------------------
int PutVarint32(char* dst, uint32_t v) {
  (void)v;
  dst[0] = '\0';
  return 1;  // 占位实现：只写一个 0 字节，测试会失败
}

int PutVarint64(char* dst, uint64_t v) {
  (void)v;
  dst[0] = '\0';
  return 1;  // 占位实现
}

// ---------------------------------------------------------------------------
// TODO(0.2) GetVarint32 / GetVarint64
//
// 输入：*input 指向一段待解析字节。
// 输出：成功时 *v 得到值，*input 前移到值之后，返回 true；
//       失败时返回 false 且 *input 保持不变。
// 约束（三条防御，测试会逐条检查）：
//   1. 剩余字节不够        -> false
//   2. 32 位版本连续 5 字节都带续位 -> false
//   3. 64 位版本连续 10 字节都带续位 -> false
// ---------------------------------------------------------------------------
bool GetVarint32(Slice* input, uint32_t* v) {
  (void)v;
  if (input->empty()) return false;
  // 占位实现：吃掉 1 字节，值写 0
  uint32_t result = static_cast<unsigned char>((*input)[0]) & 0x7f;
  input->remove_prefix(1);
  *v = result;
  return true;  // 占位实现
}

bool GetVarint64(Slice* input, uint64_t* v) {
  uint32_t tmp = 0;
  if (!GetVarint32(input, &tmp)) return false;
  *v = tmp;
  return true;  // 占位实现（只处理了 32 位范围）
}

// ---------------------------------------------------------------------------
// 以下由课程提供，完整实现
// ---------------------------------------------------------------------------

void EncodeFixed32(char* dst, uint32_t v) {
  // 固定小端：不依赖主机字节序
  dst[0] = static_cast<char>(v & 0xff);
  dst[1] = static_cast<char>((v >> 8) & 0xff);
  dst[2] = static_cast<char>((v >> 16) & 0xff);
  dst[3] = static_cast<char>((v >> 24) & 0xff);
}

void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  }
}

uint32_t DecodeFixed32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0]))) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

uint64_t DecodeFixed64(const char* p) {
  uint64_t lo = DecodeFixed32(p);
  uint64_t hi = DecodeFixed32(p + 4);
  return lo | (hi << 32);
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
  if (input->size() < len) return false;   // 防越界：不信任声明的长度
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace lsm
