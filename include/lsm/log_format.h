// WAL 的物理格式。
//
// 一条记录：
//   +--------+---------+--------+------------------+
//   | crc32  | len(2)  | type(1)|     payload      |
//   | 4 byte | 小端    |        |  (len 字节)       |
//   +--------+---------+--------+------------------+
//
// crc32 覆盖 type + len + payload（不覆盖 crc 自身）。
// 文件按 32KB 分块；块尾剩余不足 kHeaderSize 时用 0 填充，记录不跨块。
#pragma once

#include <cstdint>

namespace lsm {
namespace log {

enum RecordType : uint8_t {
  kZeroType = 0,    // 预分配文件的零填充区
  kFullType = 1,    // 一条完整记录（本课程只用这个）
  kFirstType = 2,   // 分片记录的第一片（延伸问题）
  kMiddleType = 3,  // 分片记录的中间片
  kLastType = 4,    // 分片记录的最后一片
};

static constexpr int kBlockSize = 32768;                               // 32KB
static constexpr int kHeaderSize = 7;                                  // 4 + 2 + 1
static constexpr int kMaxRecordPayload = kBlockSize - kHeaderSize;      // 32761

}  // namespace log
}  // namespace lsm
