// WAL 写入端。
//
// 语义：AddRecord 把一条逻辑记录切成 <=kMaxRecordPayload 的片段追加到文件。
// 本课程不做分片（超过上限直接拒绝），见正文 5.2 节的讨论。
#pragma once

#include <cstdint>

#include "lsm/env.h"
#include "lsm/log_format.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {
namespace log {

class Writer {
 public:
  explicit Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {}

  // 追加一条逻辑记录。返回 ok 表示"已交给 WritableFile"，
  // 不表示已落盘（落盘要另外调 dest->Sync()）。
  Status AddRecord(const Slice& record);

  int block_offset() const { return block_offset_; }

 private:
  WritableFile* dest_;
  int block_offset_;   // 当前块内已用字节数
};

}  // namespace log
}  // namespace lsm
