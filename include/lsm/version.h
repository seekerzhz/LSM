// Version：某一时刻「磁盘上有哪些 SST」的不可变快照。
//
// 「不可变」是它能被无锁读的原因：读者拿到 Version 后，它的文件列表
// 永远不会变。compaction 想改文件集合，就造一个新 Version，然后原子地
// 把 current_ 指过去。旧 Version 只要还有人引用，它引用的文件就不能删。
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "lsm/db.h"
#include "lsm/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/slice.h"
#include "lsm/status.h"
#include "lsm/version_edit.h"

namespace lsm {

class VersionSet;

class VersionSet;

class Version {
 public:
  // 每层按 smallest user_key 升序。
  // L0 特殊：文件之间 key 可以重叠，顺序是「编号降序」（新的优先）。
  std::vector<FileMetaData*> files_[kNumLevels];

  explicit Version(VersionSet* vset);
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // -------------------------------------------------------------------------
  // 按层查找。返回 OK 表示命中（value 已设置），NotFound 表示整库没有。
  //
  // 注意：这个函数**不做删除语义的判断**，它只负责「找到第一个命中的记录」。
  // 墓碑的语义在 db_impl 里处理——但这里必须保证「命中墓碑就停」，
  // 否则删除会复活。
  // -------------------------------------------------------------------------
  Status Get(const ReadOptions& opt, const LookupKey& key, std::string* value,
             GetStats* stats);

  // 收集某层中与 [smallest_user_key, largest_user_key] 重叠的文件。
  // 指针传 nullptr 表示该侧不设边界。
  void OverlapInLevel(int level, const Slice* smallest_user_key,
                      const Slice* largest_user_key,
                      std::vector<FileMetaData*>* inputs) const;

  // 为下面各层的所有文件各建一个迭代器（compaction 的输入）
  void AddIterators(std::vector<Iterator*>* iters);

  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }

  void Ref();
  void Unref();
  int refs() const { return refs_.load(std::memory_order_relaxed); }

  // 调试/测试用：这个版本里所有文件的编号
  std::vector<uint64_t> AllFileNumbers() const;

 private:
  friend class VersionSet;

  // 在某一层找「唯一可能包含 user_key」的文件（L1 及以上用，二分）
  FileMetaData* FindFileInLevel(int level, const Slice& user_key) const;

  VersionSet* vset_;
  // 必须初始化成 nullptr。新建出来、还没进链表的 Version（比如 Recover 里
  // 「从零重放」用的那个空基线版本）也走同一条 Ref/Unref 路径：引用归零时
  // 会调 RemoveVersion，而 RemoveVersion 要先判断「它到底在不在链表上」。
  // 判断靠的就是这两个指针——不初始化的话读到的是上一个人留下的垃圾，
  // 于是它会去改一个随机地址的 prev_/next_，症状是一个和版本管理
  // 完全无关的 segfault（ASan 之前只在 asan/ubsan 构建里出现）。
  Version* next_ = nullptr;
  Version* prev_ = nullptr;
  // 用原子计数：Ref 在 mu_ 内（读者线程），Unref 在锁外（读者读完 / 版本被替换）。
  // 普通的 int 在这里会真的丢引用——两个线程同时 -- 可能都读到 1，
  // 于是 refs_ 提前归零、Version 被 delete，随后别人访问它的 files_
  // 就是读到一块已经被回收的内存。症状是「偶发的压缩后丢数据」。
  std::atomic<int> refs_{0};

  // 本版本「最后一次被引用期间不能删」的文件。等 refs_ 归零才真正删盘。
  // 这正是「删除必须延迟」的实现：正在读旧版本的人不会因为 compaction
  // 删了文件而读到 ENOENT。
  std::vector<uint64_t> files_to_delete_;
};

}  // namespace lsm
