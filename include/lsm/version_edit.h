// FileMetaData 与 VersionEdit：MANIFEST 的「一句话」。
//
// MANIFEST 是一串 VersionEdit 的日志。每一行说「删掉这几个文件、加上这几个
// 文件、当前 seq 是多少」。把它们按顺序重放，就重建出当前的文件集合。
//
// 为什么不直接存一份「文件清单快照」：那样每次改动都要重写整个清单
// （几千个文件就是几 MB），而 append 一条 edit 只有几十字节。
// 这是把「快照」换成「增量日志」的经典取舍——代价是启动要重放。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lsm/dbformat.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// 一个 SST 的元数据。refs 是「有多少个 Version 在用它」——
// 只有在所有引用它的 Version 都消失后，文件才能从磁盘上删掉。
//
// 为什么 refs 是 atomic 而不是 int：
//   这个对象被**多个 Version 共享**（LogAndApply 只复制指针，不复制对象）。
//   两个不同的线程会同时改它的计数——
//     前台线程在 LogAndApply 里为新版本 `f->refs++`；
//     后台线程在 Version::Unref 里销毁旧版本时 `--f->refs`。
//   这两条路径不共用一把锁（版本销毁本来就要能在锁外发生），
//   所以普通 int 的 `++`/`--` 是真正的数据竞争：两边都读到 1，
//   一个写回 2、一个写回 0，结果是提前 delete（另一个版本立刻悬垂）
//   或者永远不 delete（文件泄漏）。TSan 会直接指到这一行。
struct FileMetaData {
  std::atomic<int> refs{0};
  uint64_t number = 0;
  uint64_t file_size = 0;
  InternalKey smallest;
  InternalKey largest;

  FileMetaData() = default;
  // atomic 成员不可拷贝，所以手写一份：只搬元数据，引用计数从源对象读一个值。
  FileMetaData(const FileMetaData& o)
      : refs(o.refs.load(std::memory_order_relaxed)),
        number(o.number),
        file_size(o.file_size),
        smallest(o.smallest),
        largest(o.largest) {}
  FileMetaData& operator=(const FileMetaData& o) {
    refs.store(o.refs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    number = o.number;
    file_size = o.file_size;
    smallest = o.smallest;
    largest = o.largest;
    return *this;
  }

  // 这个文件的 key 范围是否覆盖 user_key
  bool Overlaps(const Slice& user_key) const {
    return smallest.Valid() && largest.Valid() &&
           smallest.user_key().compare(user_key) <= 0 &&
           largest.user_key().compare(user_key) >= 0;
  }
};

class VersionEdit {
 public:
  VersionEdit() { Clear(); }

  void Clear();

  void SetComparatorName(const Slice& name);
  void SetLogNumber(uint64_t num);
  void SetPrevLogNumber(uint64_t num);
  void SetNextFile(uint64_t num);
  void SetLastSequence(SequenceNumber seq);
  // 记录「这一层的下次 compaction 从这里开始」，用于轮转覆盖 key 空间
  void SetCompactPointer(int level, const InternalKey& key);

  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest);
  void DeleteFile(int level, uint64_t file);

  // 编码成一条 MANIFEST 记录的 payload
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

  // VersionSet::LogAndApply 需要读这些字段
  bool has_comparator() const { return has_comparator_; }
  const std::string& comparator() const { return comparator_; }
  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }
  bool has_prev_log_number() const { return has_prev_log_number_; }
  uint64_t prev_log_number() const { return prev_log_number_; }
  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }
  bool has_last_sequence() const { return has_last_sequence_; }
  SequenceNumber last_sequence() const { return last_sequence_; }

  using DeletedFileSet = std::vector<std::pair<int, uint64_t>>;
  using NewFileList = std::vector<std::pair<int, FileMetaData>>;
  using CompactPointerList = std::vector<std::pair<int, InternalKey>>;

  const DeletedFileSet& deleted_files() const { return deleted_files_; }
  const NewFileList& new_files() const { return new_files_; }
  const CompactPointerList& compact_pointers() const { return compact_pointers_; }

 private:
  // tag-length-value：每个字段一个 tag 字节。新增字段时老版本读到不认识的
  // tag 会报错（本课程不跳过），但至少格式是自描述的、可扩展的。
  enum Tag {
    kComparator = 1,
    kLogNumber = 2,
    kNextFileNumber = 3,
    kLastSequence = 4,
    kCompactPointer = 5,
    kDeletedFile = 6,
    kNewFile = 7,
    kPrevLogNumber = 9,
  };

  bool has_comparator_ = false;
  std::string comparator_;
  bool has_log_number_ = false;
  uint64_t log_number_ = 0;
  bool has_prev_log_number_ = false;
  uint64_t prev_log_number_ = 0;
  bool has_next_file_number_ = false;
  uint64_t next_file_number_ = 0;
  bool has_last_sequence_ = false;
  SequenceNumber last_sequence_ = 0;

  CompactPointerList compact_pointers_;
  DeletedFileSet deleted_files_;
  NewFileList new_files_;
};

}  // namespace lsm
