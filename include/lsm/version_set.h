// VersionSet：版本集合 + MANIFEST 的管理者。
//
// 它维护两样东西：
//   1. 一串不可变的 Version（current_ 是当前那个，dummy_versions_ 是所有活着的）
//   2. MANIFEST 文件（一串 VersionEdit 的日志）
//
// 所有会改变文件集合的操作（flush 生成新 SST、compaction 换文件）都走
// LogAndApply：先落盘 MANIFEST，再换内存指针。顺序反了，崩溃后磁盘和内存
// 就不一致，而且无法判断以谁为准。
#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "lsm/compaction.h"
#include "lsm/db.h"
#include "lsm/dbformat.h"
#include "lsm/log_writer.h"
#include "lsm/status.h"
#include "lsm/table_cache.h"
#include "lsm/version.h"
#include "lsm/version_edit.h"

namespace lsm {

class VersionSet {
 public:
  VersionSet(const Options& options, const std::string& dbname, TableCache* table_cache);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // 从 CURRENT + MANIFEST 重建版本。成功后：
  //   current() 可用、log_number()/last_sequence() 已就位、descriptor 可继续追加。
  // 全新库（CURRENT 不存在）时自动创建初始 MANIFEST。
  Status Recover();

  // 追加一条 edit 并切换到新版本。
  // mu == nullptr 表示「调用者已经持有 DB 的锁」，不要再拿；
  // 传 &mutex 表示「帮我把整段过程保护起来」。
  // 这个显式参数是为了让「递归加锁」这种错误在编译期就看得见。
  Status LogAndApply(VersionEdit* edit, std::mutex* mu);

  Version* current() const { return current_; }

  // 选一轮 compaction；没有可做的返回 nullptr。
  Compaction* PickCompaction();
  bool NeedsCompaction() const;

  // 编号分配。SST / WAL / MANIFEST 共用同一个计数器，
  // 这样「文件名 -> 新不新」可以直接比大小，L0 的排序就靠它。
  uint64_t NewFileNumber() { return next_file_number_++; }
  void ReuseFileNumber(uint64_t number) {
    if (next_file_number_ == number + 1) next_file_number_ = number;
  }
  // 恢复时把计数器顶到「已见过最大值 + 1」之上，避免复用旧编号覆盖文件
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }

  uint64_t manifest_file_number() const { return manifest_file_number_; }
  uint64_t log_number() const { return log_number_; }
  uint64_t prev_log_number() const { return prev_log_number_; }
  void SetLogNumber(uint64_t n) { log_number_ = n; }
  void SetPrevLogNumber(uint64_t n) { prev_log_number_ = n; }

  SequenceNumber LastSequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }

  const InternalKeyComparator& icmp() const { return icmp_; }
  TableCache* table_cache() const { return table_cache_; }
  const std::string& dbname() const { return dbname_; }
  const Options& options() const { return options_; }

  // 当前版本里所有 SST 的总字节数与个数
  void GetLiveFiles(std::vector<uint64_t>* numbers, uint64_t* total_bytes) const;

  // 所有**活着的** Version 引用的文件编号。
  // DeleteObsoleteFiles 用它判断「这个 sst 还有没有人要」——
  // 只看 current() 是不够的：一个正在扫表的读者可能还持有旧版本。
  void AddLiveFiles(std::set<uint64_t>* live) const;

  uint64_t next_file_number() const { return next_file_number_; }

  static std::string DescriptorFileName(const std::string& dbname, uint64_t number);
  static std::string CurrentFileName(const std::string& dbname);

 private:
  // Version::Unref 需要调 RemoveVersion（摘链 + delete）
  friend class Version;

  // 把「一串 edit」折叠成一个 Version 的辅助类
  struct Builder;

  // 读 CURRENT -> 得到 MANIFEST 文件名；找不到返回 NotFound
  Status ReadCurrentFile(std::string* manifest_name, uint64_t* manifest_number);

  // 新建 MANIFEST 并把当前状态写成一条快照 edit，然后更新 CURRENT
  Status WriteSnapshot();

  // 把 current_ 换成 v（接管引用）
  void AppendVersion(Version* v);

  // 从版本链表摘掉并 delete（内部持 list_mu_）。
  // 只由 Version::Unref 在 refs_ 归零时调用。
  void RemoveVersion(Version* v);

  Status SetCurrentFile(uint64_t descriptor_number);

  Env* env_;
  Options options_;
  std::string dbname_;
  TableCache* table_cache_;
  InternalKeyComparator icmp_;

  Version* current_ = nullptr;
  // 所有活着的 Version 的双向链表头（哨兵节点，不装实际文件）。
  // 它让「哪些文件还被引用」可查 —— 这是延迟删除的前提。
  Version* dummy_versions_ = nullptr;

  // 版本链表的专用锁：增（AppendVersion）、删（RemoveVersion）、
  // 查（AddLiveFiles）都必须持它。
  //
  // 为什么不能用 mu_：读者线程在**锁外** Unref（Get 返回之后才 Unref，
  // 目的是让临界区里不做任何可能阻塞的事），那条路径会走到删除 Version。
  // 如果删除时不加锁，压缩线程正好在 AddLiveFiles 遍历链表，就会读到
  // 已经被回收的节点——表现是「活文件集合少了一个」，于是刚写好的 SST
  // 被当成垃圾删掉。这个 bug 只在并发下出现，症状是偶发丢数据。
  mutable std::mutex list_mu_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  WritableFile* descriptor_file_ = nullptr;
  log::Writer* descriptor_log_ = nullptr;

  std::string compact_pointer_[kNumLevels];
};

}  // namespace lsm
