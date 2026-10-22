#include "lsm/log_reader.h"

#include <cstring>

#include "lsm/coding.h"
#include "lsm/crc32.h"

namespace lsm {
namespace log {

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      initial_offset_(initial_offset),
      last_record_offset_(0),
      block_start_offset_(0),
      eof_(false) {}

bool Reader::ReadBlock() {
  if (eof_) return false;
  char buf[kBlockSize];
  Slice result;
  Status s = file_->Read(kBlockSize, &result, buf);
  if (!s.ok()) {
    if (reporter_ != nullptr && reporter_->status != nullptr) *reporter_->status = s;
    eof_ = true;
    return false;
  }
  if (result.empty()) {
    eof_ = true;
    return false;
  }
  block_start_offset_ += buffer_.size();   // 上一块已消费的长度计入偏移
  backing_store_.assign(result.data(), result.size());
  buffer_ = Slice(backing_store_);
  return true;
}

// ---------------------------------------------------------------------------
// TODO(1.9) Reader::ReadRecord
//
// 输入：无（从当前 buffer_ 继续读）
// 输出：成功 true 且 *record 指向内部数据；失败 false
// 约束（按顺序判断，每一条都对应一种真实故障）：
//
//   1. buffer_ 里剩余 < kHeaderSize：
//        - 且这是文件最后一块（后面没有数据了）-> 视为填充/尾截断，返回 false，*status 保持 ok
//        - 否则 -> 跳过该块剩余部分，ReadBlock() 重新开始
//   2. 解析 crc(4) / len(2) / type(1)
//   3. type == kZeroType -> 整个块是预分配的零区，跳过后 ReadBlock()
//   4. len > kMaxRecordPayload -> 损坏：*status = Corruption("bad length")
//   5. 剩余不足 kHeaderSize + len：
//        - 说明记录被截断在文件中间 -> 通常发生在"最后一条写了一半"，
//          返回 false 且 **不要** 设置 status（让上层继续启动，接受丢最后一条）
//   6. checksum_ 为真时校验 CRC（覆盖 type+len+payload）：
//        不匹配 -> *status = Corruption("checksum mismatch")
//   7. 成功：*record 指向 payload，buffer_ 前移 kHeaderSize+len，
//      更新 last_record_offset_
// ---------------------------------------------------------------------------
bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  (void)scratch;
  *record = Slice();
  if (!ReadBlock()) return false;
  // 占位实现：把整块当成一条记录返回（内容不对，但至少不是空指针）
  *record = buffer_;
  buffer_.clear();
  return true;
}

}  // namespace log
}  // namespace lsm
