// 字节编解码：VarInt（变长整数）、Fixed（定长小端）、LengthPrefixed。
//
// VarInt 布局（以 32 位为例）：
//   每个字节低 7 位有效，最高位 1 表示"后面还有字节"，0 表示"这是最后一字节"。
//   0      -> 00
//   127    -> 7f
//   128    -> 80 01
//   300    -> ac 02
//   16384  -> 80 80 01
//   UINT32_MAX -> ff ff ff ff 0f
#pragma once

#include <cstdint>
#include <string>

#include "lsm/slice.h"

namespace lsm {

// 把一个 32 位无符号整数编码到 dst。dst 必须至少有 5 字节可写空间。
// 返回写入的字节数。
int PutVarint32(char* dst, uint32_t v);

// 同上，64 位版本，dst 至少 10 字节。
int PutVarint64(char* dst, uint64_t v);

// 从 *input 解析一个 VarInt32。
// 成功：*v 得到值，*input 前移到值之后，返回 true。
// 失败：返回 false 且 *input 不变。失败条件包括
//   - 剩余字节不足
//   - 续位标志连续出现超过 4 次（第 5 字节仍带 0x80）
// 注意：不做「第 5 字节高 4 位必须为 0」的严格校验（LevelDB 也不做），
//       但你需要在文档里说明这种输入会被静默截断到 32 位。
bool GetVarint32(Slice* input, uint32_t* v);
bool GetVarint64(Slice* input, uint64_t* v);

// 定长小端编解码。dst/src 必须有 4（或 8）字节。
void EncodeFixed32(char* dst, uint32_t v);
void EncodeFixed64(char* dst, uint64_t v);
uint32_t DecodeFixed32(const char* p);
uint64_t DecodeFixed64(const char* p);

// 带长度前缀的字符串：[VarInt32 len][payload]
void PutLengthPrefixedSlice(std::string* dst, const Slice& v);

// 成功时 result 指向 input 内部（不拷贝），input 前移到 payload 之后。
// 若声明长度超过剩余字节数，返回 false 且不改动 input/result。
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// 便利函数：返回编码 v 所需的字节数（1..5）
int VarintLength(uint64_t v);

}  // namespace lsm
