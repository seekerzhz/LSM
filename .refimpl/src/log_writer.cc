// 参考实现（教师版），仅用于验证骨架。
#include "lsm/log_writer.h"

#include <cstring>

#include "lsm/coding.h"
#include "lsm/crc32.h"

namespace lsm {
namespace log {

Status Writer::AddRecord(const Slice& record) {
  if (record.size() > static_cast<size_t>(kMaxRecordPayload)) {
    return Status::Invalid("record too large");
  }

  const int left = kBlockSize - block_offset_;
  if (left < kHeaderSize + static_cast<int>(record.size())) {
    // 块尾剩余空间不足：用 0 填充，记录从下一块开始（不跨块）
    if (left > 0) {
      const std::string pad(static_cast<size_t>(left), '\0');
      const Status s = dest_->Append(Slice(pad));
      if (!s.ok()) return s;
    }
    block_offset_ = 0;
  }

  char header[kHeaderSize];
  header[4] = static_cast<char>(record.size() & 0xff);
  header[5] = static_cast<char>((record.size() >> 8) & 0xff);
  header[6] = static_cast<char>(kFullType);

  // crc 覆盖 type + len + payload（注意顺序：先 type，再 len，再 payload）
  uint32_t crc = Crc32c(header + 6, 1);
  crc = Crc32cExtend(crc, header + 4, 2);
  crc = Crc32cExtend(crc, record.data(), record.size());
  EncodeFixed32(header, crc);

  std::string buf;
  buf.reserve(static_cast<size_t>(kHeaderSize) + record.size());
  buf.append(header, kHeaderSize);
  buf.append(record.data(), record.size());

  Status s = dest_->Append(Slice(buf));
  if (!s.ok()) return s;
  block_offset_ += kHeaderSize + static_cast<int>(record.size());
  return Status::OK();
}

}  // namespace log
}  // namespace lsm
