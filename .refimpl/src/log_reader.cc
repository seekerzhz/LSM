// 参考实现（教师版），仅用于验证骨架。
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
  block_start_offset_ += buffer_.size();
  backing_store_.assign(result.data(), result.size());
  buffer_ = Slice(backing_store_);
  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  (void)scratch;
  while (true) {
    if (buffer_.size() < static_cast<size_t>(kHeaderSize)) {
      if (!ReadBlock()) return false;
      continue;
    }

    const uint32_t crc = DecodeFixed32(buffer_.data());
    const uint32_t len = static_cast<unsigned char>(buffer_[4]) |
                         (static_cast<uint32_t>(static_cast<unsigned char>(buffer_[5])) << 8);
    const uint8_t type = static_cast<uint8_t>(buffer_[6]);

    if (type == kZeroType) {
      // 整块是预分配的零区，跳到下一块
      buffer_.clear();
      if (!ReadBlock()) return false;
      continue;
    }

    if (len > static_cast<uint32_t>(kMaxRecordPayload)) {
      if (reporter_ != nullptr && reporter_->status != nullptr) {
        *reporter_->status = Status::Corruption("bad record length");
      }
      return false;
    }

    if (buffer_.size() < static_cast<size_t>(kHeaderSize) + len) {
      // 文件末尾的记录不完整：这是崩溃的正常残留，静默丢弃（不设 status）
      const bool had_more = ReadBlock();
      if (!had_more) return false;
      continue;
    }

    if (checksum_) {
      uint32_t actual = Crc32c(buffer_.data() + 6, 1);
      actual = Crc32cExtend(actual, buffer_.data() + 4, 2);
      actual = Crc32cExtend(actual, buffer_.data() + kHeaderSize, len);
      if (actual != crc) {
        if (reporter_ != nullptr && reporter_->status != nullptr) {
          *reporter_->status = Status::Corruption("checksum mismatch");
        }
        return false;
      }
    }

    last_record_offset_ = block_start_offset_ + (backing_store_.size() - buffer_.size());
    *record = Slice(buffer_.data() + kHeaderSize, len);
    buffer_.remove_prefix(static_cast<size_t>(kHeaderSize) + len);
    return true;
  }
}

}  // namespace log
}  // namespace lsm
