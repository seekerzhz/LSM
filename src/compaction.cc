#include "lsm/compaction.h"

#include "lsm/version.h"

namespace lsm {

uint64_t MaxBytesForLevel(const Options& options, int level) {
  if (level <= 0) return 0;
  // L1 能放 10 个最大文件，L2 放 100 个……
  uint64_t result = static_cast<uint64_t>(options.max_file_size) * 10;
  for (int l = 1; l < level; ++l) {
    result *= 10;
  }
  return result;
}

uint64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  uint64_t sum = 0;
  for (const FileMetaData* f : files) sum += f->file_size;
  return sum;
}

Compaction::Compaction(const Options& options, int level)
    : options_(options), level_(level), max_output_file_size_(options.max_file_size) {}

Compaction::~Compaction() = default;

uint64_t Compaction::TotalInputSize() const {
  return TotalFileSize(inputs_[0]) + TotalFileSize(inputs_[1]);
}

bool Compaction::IsTrivialMove() const {
  // L0 不能这么做：L0 的文件之间可能重叠，直接挪到 L1 会破坏
  // 「L1 内文件互不重叠」的不变量。
  if (level_ == 0) return false;
  // 本层只有一个文件，且下一层没有任何重叠文件 → 直接改元数据挪下去
  return inputs_[0].size() == 1 && inputs_[1].empty();
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // 只看 level_+2 及更深：level_ 和 level_+1 的文件都在本次输入里，
  // 它们的旧版本会被这次归并一起处理掉，不需要「下面还有没有」。
  for (int lvl = level_ + 2; lvl < kNumLevels; ++lvl) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    const int n = static_cast<int>(files.size());
    // 游标只前进：调用方保证 user_key 升序，所以已经扫过的文件
    // 不可能再覆盖后面的 key
    while (level_ptrs_[lvl] < n && user_key.compare(files[level_ptrs_[lvl]]->largest.user_key()) > 0) {
      ++level_ptrs_[lvl];
    }
    if (level_ptrs_[lvl] < n && user_key.compare(files[level_ptrs_[lvl]]->smallest.user_key()) >= 0) {
      return false;  // 更深层还有这个 key
    }
  }
  return true;
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; ++which) {
    for (FileMetaData* f : inputs_[which]) {
      edit->DeleteFile(level_ + which, f->number);
    }
  }
}

}  // namespace lsm
