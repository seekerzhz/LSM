#include "lsm/version.h"

#include <algorithm>
#include <cassert>

#include "lsm/table_cache.h"
#include "lsm/version_set.h"

namespace lsm {

Version::Version(VersionSet* vset) : vset_(vset) {}

Version::~Version() {
  // 摘链由 VersionSet::RemoveVersion 在锁内完成。到这里必须已经摘掉了：
  // 否则说明有人绕过 RemoveVersion 直接 delete，链表会留下悬垂指针。
  assert(next_ == nullptr && "Version destroyed while still linked");

  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : files_[level]) {
      // 同一个 FileMetaData 可能被多个 Version 共享（LogAndApply 复制指针时
      // refs++），所以这里只是减引用，不是无条件 delete。
      assert(f->refs > 0);
      --f->refs;
      if (f->refs == 0) delete f;
    }
    files_[level].clear();
  }
}

void Version::Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// TODO(3.5) Version::Unref 与延迟删除
//
// 约束：
//   1. --refs_；仍 > 0 就直接返回
//   2. 为 0 时，逐个删 files_to_delete_ 里的文件：
//      先 table_cache->Evict(number)（关 fd），再 env->RemoveFile
//      忽略「文件不存在」——它可能已经被上一轮删过了
//   3. 最后 delete this
//   4. 调试构建里断言 refs_ >= 0（变负说明多 Unref 了）
//
// 为什么必须等到 refs_ == 0 才删：正在用这个 Version 的读者手里可能有
// 指向这些文件的迭代器。提前删掉，读者会在下一次 Next() 时拿到 ENOENT。
// 这个 bug 只在并发 + compaction 时出现，单线程测试永远抓不到。
// ---------------------------------------------------------------------------
void Version::Unref() {
  // 占位实现：只把计数减一。归零之后**不释放、不删文件、不摘链**。
  //
  // 为什么这么挖：如果连减计数都不做，refs_ 单调上涨，你会看到「什么都没
  // 发生」，毫无线索。减了之后，归零时刻的症状才开始暴露：
  //   - 旧 SST 永远删不掉（files_to_delete_ 没人处理，磁盘只增不减）
  //   - Version 本身永不释放（LSan 会报一串泄漏）
  //   - 版本链表越来越长（AddLiveFiles 越来越慢）
  //
  // 按上面 4 步填。第 2 步的顺序别写反：先 TableCache::Evict 关 fd，
  // 再 RemoveFile；反过来在 Linux 上也能删成功，但空间要等最后一个
  // 句柄关闭才回收，`du` 和 `df` 会对不上。
  refs_.fetch_sub(1);
}

FileMetaData* Version::FindFileInLevel(int level, const Slice& user_key) const {
  const std::vector<FileMetaData*>& files = files_[level];
  if (files.empty()) return nullptr;

  // 找最后一个 smallest <= user_key 的文件。因为层内文件按 smallest 升序
  // 且互不重叠，这个文件就是唯一可能的候选。
  size_t lo = 0;
  size_t hi = files.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (files[mid]->smallest.user_key().compare(user_key) <= 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return nullptr;
  FileMetaData* f = files[lo - 1];
  // smallest <= key 但 largest < key：落在两个文件之间的空隙里
  if (f->largest.user_key().compare(user_key) >= 0) return f;
  return nullptr;
}

// ---------------------------------------------------------------------------
// TODO(3.1) Version::Get
//
// 约束：
//   1. L0：文件按 number **降序**（新的在前）遍历。先用 key 范围快速排除，
//      再交给 Table::InternalGet（它内部再做 Bloom 和块内查找）。
//      **第一个命中的就是最终答案**，命中墓碑也不能继续往下找。
//   2. L1 及以上：层内文件不重叠，FindFileInLevel 二分定位到唯一一个，
//      查一次；命中即返回，没命中就换下一层。
//   3. 每一层最多读一个块 —— 这是 5.1 那个读放大承诺的实现。
//
// 想一想：如果 L0 有 8 个文件都和这个 key 重叠，最坏要读 8 次。
// 这就是为什么 L0 文件数必须被 compaction 压住（kL0_CompactionTrigger）。
// ---------------------------------------------------------------------------
Status Version::Get(const ReadOptions& opt, const LookupKey& key, std::string* value,
                    GetStats* stats) {
  (void)opt;
  (void)key;
  (void)value;
  (void)stats;
  // 占位实现：一律说「哪一层都没有」。
  // 后果：数据写进 SST 之后就再也读不回来 —— 重启后 Put 过的 key 全 NotFound。
  // 按上面 3 条写；最要紧的是 L0 一定要按 number 降序遍历（新的先查），
  // 以及命中墓碑立刻返回 NotFound，不能继续往更旧的层找。
  return Status::NotFound("Version::Get 还没实现（TODO 3.1）");
}

void Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key,
                             std::vector<FileMetaData*>* inputs) const {
  const std::vector<FileMetaData*>& files = files_[level];
  if (files.empty()) return;

  if (level == 0) {
    for (FileMetaData* f : files) {
      if (smallest_user_key != nullptr &&
          f->largest.user_key().compare(*smallest_user_key) < 0) {
        continue;
      }
      if (largest_user_key != nullptr &&
          f->smallest.user_key().compare(*largest_user_key) > 0) {
        continue;
      }
      inputs->push_back(f);
    }
    return;
  }

  // L1+：按 smallest 排序，先从「第一个可能重叠」的位置起跳，再往后扫
  size_t lo = 0;
  size_t hi = files.size();
  if (smallest_user_key != nullptr) {
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (files[mid]->largest.user_key().compare(*smallest_user_key) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
  }
  for (size_t i = lo; i < files.size(); ++i) {
    if (largest_user_key != nullptr &&
        files[i]->smallest.user_key().compare(*largest_user_key) > 0) {
      break;
    }
    inputs->push_back(files[i]);
  }
}

void Version::AddIterators(std::vector<Iterator*>* iters) {
  TableCache* tc = vset_->table_cache();
  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : files_[level]) {
      iters->push_back(tc->NewIterator(f->number, f->file_size));
    }
  }
}

std::vector<uint64_t> Version::AllFileNumbers() const {
  std::vector<uint64_t> out;
  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : files_[level]) out.push_back(f->number);
  }
  return out;
}

}  // namespace lsm
