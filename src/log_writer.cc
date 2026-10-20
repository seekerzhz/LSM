#include "lsm/log_writer.h"

#include <cstring>

#include "lsm/coding.h"
#include "lsm/crc32.h"

namespace lsm {
namespace log {

// ---------------------------------------------------------------------------
// TODO(1.8) Writer::AddRecord
//
// 输入：record（payload，长度 <= kMaxRecordPayload）
// 输出：Status
// 约束：
//   1. 若 block_offset_ + kHeaderSize + record.size() > kBlockSize：
//      用 '\0' 填充块尾（kBlockSize - block_offset_ 字节），block_offset_ = 0
//   2. payload 超过 kMaxRecordPayload -> Status::Invalid("record too large")
//   3. 写 7 字节头：
//        - crc = Crc32cExtend(0, 从 type 字节开始的 len+1 字节 + payload)
//          注意：具体覆盖范围是 (type, len, payload)，也就是先算 type(1 字节)
//          再 extend 长度字段(2 字节, 小端) 再 extend payload。
//          顺序必须与 LogReader 校验时一致，否则永远对不上。
//        - len 小端 2 字节
//        - type = kFullType
//   4. 写 payload，更新 block_offset_
// ---------------------------------------------------------------------------
Status Writer::AddRecord(const Slice& record) {
  (void)record;
  // 占位实现：什么都不写。测试 Wal.RoundTrip100 会失败。
  return Status::OK();
}

}  // namespace log
}  // namespace lsm
