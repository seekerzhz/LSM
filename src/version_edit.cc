#include "lsm/version_edit.h"

#include <cstdio>

#include "lsm/coding.h"

namespace lsm {

void VersionEdit::Clear() {
  has_comparator_ = false;
  comparator_.clear();
  has_log_number_ = false;
  log_number_ = 0;
  has_prev_log_number_ = false;
  prev_log_number_ = 0;
  has_next_file_number_ = false;
  next_file_number_ = 0;
  has_last_sequence_ = false;
  last_sequence_ = 0;
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::SetComparatorName(const Slice& name) {
  has_comparator_ = true;
  comparator_ = name.ToString();
}
void VersionEdit::SetLogNumber(uint64_t num) {
  has_log_number_ = true;
  log_number_ = num;
}
void VersionEdit::SetPrevLogNumber(uint64_t num) {
  has_prev_log_number_ = true;
  prev_log_number_ = num;
}
void VersionEdit::SetNextFile(uint64_t num) {
  has_next_file_number_ = true;
  next_file_number_ = num;
}
void VersionEdit::SetLastSequence(SequenceNumber seq) {
  has_last_sequence_ = true;
  last_sequence_ = seq;
}
void VersionEdit::SetCompactPointer(int level, const InternalKey& key) {
  compact_pointers_.push_back({level, key});
}

void VersionEdit::AddFile(int level, uint64_t file, uint64_t file_size,
                          const InternalKey& smallest, const InternalKey& largest) {
  FileMetaData f;
  f.number = file;
  f.file_size = file_size;
  f.smallest = smallest;
  f.largest = largest;
  new_files_.push_back({level, f});
}

void VersionEdit::DeleteFile(int level, uint64_t file) {
  deleted_files_.push_back({level, file});
}

void VersionEdit::EncodeTo(std::string* dst) const {
  char buf[16];

  if (has_comparator_) {
    dst->append(buf, PutVarint32(buf, kComparator));
    PutLengthPrefixedSlice(dst, Slice(comparator_));
  }
  if (has_log_number_) {
    dst->append(buf, PutVarint32(buf, kLogNumber));
    dst->append(buf, PutVarint64(buf, log_number_));
  }
  if (has_prev_log_number_) {
    dst->append(buf, PutVarint32(buf, kPrevLogNumber));
    dst->append(buf, PutVarint64(buf, prev_log_number_));
  }
  if (has_next_file_number_) {
    dst->append(buf, PutVarint32(buf, kNextFileNumber));
    dst->append(buf, PutVarint64(buf, next_file_number_));
  }
  if (has_last_sequence_) {
    dst->append(buf, PutVarint32(buf, kLastSequence));
    dst->append(buf, PutVarint64(buf, last_sequence_));
  }

  for (const auto& p : compact_pointers_) {
    dst->append(buf, PutVarint32(buf, kCompactPointer));
    dst->append(buf, PutVarint32(buf, static_cast<uint32_t>(p.first)));
    PutLengthPrefixedSlice(dst, p.second.Encode());
  }
  for (const auto& p : deleted_files_) {
    dst->append(buf, PutVarint32(buf, kDeletedFile));
    dst->append(buf, PutVarint32(buf, static_cast<uint32_t>(p.first)));
    dst->append(buf, PutVarint64(buf, p.second));
  }
  for (const auto& p : new_files_) {
    const FileMetaData& f = p.second;
    dst->append(buf, PutVarint32(buf, kNewFile));
    dst->append(buf, PutVarint32(buf, static_cast<uint32_t>(p.first)));
    dst->append(buf, PutVarint64(buf, f.number));
    dst->append(buf, PutVarint64(buf, f.file_size));
    PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode());
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  const char* msg = nullptr;

  uint32_t tag = 0;
  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kComparator: {
        Slice str;
        if (GetLengthPrefixedSlice(&input, &str)) {
          comparator_ = str.ToString();
          has_comparator_ = true;
        } else {
          msg = "comparator name";
        }
        break;
      }
      case kLogNumber: {
        uint64_t v = 0;
        if (GetVarint64(&input, &v)) {
          log_number_ = v;
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;
      }
      case kPrevLogNumber: {
        uint64_t v = 0;
        if (GetVarint64(&input, &v)) {
          prev_log_number_ = v;
          has_prev_log_number_ = true;
        } else {
          msg = "previous log number";
        }
        break;
      }
      case kNextFileNumber: {
        uint64_t v = 0;
        if (GetVarint64(&input, &v)) {
          next_file_number_ = v;
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;
      }
      case kLastSequence: {
        uint64_t v = 0;
        if (GetVarint64(&input, &v)) {
          last_sequence_ = v;
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;
      }
      case kCompactPointer: {
        uint32_t level = 0;
        Slice key;
        if (GetVarint32(&input, &level) && GetLengthPrefixedSlice(&input, &key)) {
          InternalKey ik;
          if (ik.DecodeFrom(key)) {
            compact_pointers_.push_back({static_cast<int>(level), ik});
          } else {
            msg = "compaction pointer key";
          }
        } else {
          msg = "compaction pointer";
        }
        break;
      }
      case kDeletedFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        if (GetVarint32(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.push_back({static_cast<int>(level), number});
        } else {
          msg = "deleted file";
        }
        break;
      }
      case kNewFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        uint64_t file_size = 0;
        Slice smallest;
        Slice largest;
        if (GetVarint32(&input, &level) && GetVarint64(&input, &number) &&
            GetVarint64(&input, &file_size) &&
            GetLengthPrefixedSlice(&input, &smallest) &&
            GetLengthPrefixedSlice(&input, &largest)) {
          FileMetaData f;
          f.number = number;
          f.file_size = file_size;
          if (!f.smallest.DecodeFrom(smallest) || !f.largest.DecodeFrom(largest)) {
            msg = "file metadata key";
          } else {
            new_files_.push_back({static_cast<int>(level), f});
          }
        } else {
          msg = "new-file entry";
        }
        break;
      }
      default:
        msg = "unknown tag";
        break;
    }
  }

  if (msg == nullptr && !input.empty()) msg = "invalid tag";

  return msg == nullptr ? Status::OK()
                        : Status::Corruption(std::string("VersionEdit: ") + msg);
}

std::string VersionEdit::DebugString() const {
  std::string r;
  char buf[64];
  r += "[";
  if (has_comparator_) r += "comparator=" + comparator_ + " ";
  if (has_log_number_) {
    std::snprintf(buf, sizeof(buf), "log=%llu ",
                  static_cast<unsigned long long>(log_number_));
    r += buf;
  }
  if (has_next_file_number_) {
    std::snprintf(buf, sizeof(buf), "next=%llu ",
                  static_cast<unsigned long long>(next_file_number_));
    r += buf;
  }
  if (has_last_sequence_) {
    std::snprintf(buf, sizeof(buf), "seq=%llu ",
                  static_cast<unsigned long long>(last_sequence_));
    r += buf;
  }
  for (const auto& p : deleted_files_) {
    std::snprintf(buf, sizeof(buf), "del(%d,%llu) ", p.first,
                  static_cast<unsigned long long>(p.second));
    r += buf;
  }
  for (const auto& p : new_files_) {
    std::snprintf(buf, sizeof(buf), "add(%d,%llu,%llu) ", p.first,
                  static_cast<unsigned long long>(p.second.number),
                  static_cast<unsigned long long>(p.second.file_size));
    r += buf;
  }
  r += "]";
  return r;
}

}  // namespace lsm
