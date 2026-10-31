// Compaction：一次「把某些文件读出来、归并、写成新文件」的描述。
//
// 它只是一个输入清单，不含执行逻辑。真正干活的是 DBImpl::CompactOnce。
// 这样拆分的好处是：「选哪些文件」可以单独测试（test_version.cc 里的
// PickCompaction 用例），不需要真的写盘。
#pragma once

#include <cstdint>
#include <vector>

#include "lsm/db.h"
#include "lsm/dbformat.h"
#include "lsm/version_edit.h"

namespace lsm {

class Version;

// level 层的容量上限（字节）。每层是上一层的 10 倍：
// L1 = 10 * max_file_size，L2 = 100 * max_file_size ...
// 这个增长因子决定了「一层顶多被写放大几次」——10 倍意味着平均每层
// 只重写 1/10 的数据，写放大约等于层数 × 10/(10-1)。
uint64_t MaxBytesForLevel(const Options& options, int level);

uint64_t TotalFileSize(const std::vector<FileMetaData*>& files);

class Compaction {
 public:
  Compaction(const Options& options, int level);
  ~Compaction();

  Compaction(const Compaction&) = delete;
  Compaction& operator=(const Compaction&) = delete;

  int level() const { return level_; }
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }
  uint64_t TotalInputSize() const;

  Version* input_version() const { return input_version_; }

  // which: 0 = 本层输入，1 = 下一层输入
  std::vector<FileMetaData*>& inputs(int which) { return inputs_[which]; }
  const std::vector<FileMetaData*>& inputs(int which) const { return inputs_[which]; }

  // 输入能否只靠「改元数据」完成：本层一个文件、下层不重叠 → 直接往下挪一层。
  // 这是 compaction 里最便宜的情况，一次 IO 都不做。
  bool IsTrivialMove() const;

  // 这个 user_key 在 level_+2 及更深的层里还有没有更旧的版本。
  // 返回 true 表示「没有」，于是这条墓碑可以丢掉。
  //
  // **必须按 user_key 升序调用**：内部用 level_ptrs_ 游标做线性推进，
  // 乱序调用会让游标失效、返回错误结果，症状是「删掉的数据又出现」。
  bool IsBaseLevelForKey(const Slice& user_key);

  // 把这轮 compaction 的输入文件写进 edit 的 DeleteFile
  void AddInputDeletions(VersionEdit* edit);

 private:
  friend class VersionSet;

  Options options_;
  int level_;
  uint64_t max_output_file_size_;
  Version* input_version_ = nullptr;
  std::vector<FileMetaData*> inputs_[2];

  // IsBaseLevelForKey 的有序扫描游标：每层扫到第几个文件了
  int level_ptrs_[kNumLevels] = {0};
};

}  // namespace lsm
