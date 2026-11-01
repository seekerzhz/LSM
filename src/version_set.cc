#include "lsm/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>

#include "lsm/coding.h"
#include "lsm/log_reader.h"

namespace lsm {
namespace {

// "MANIFEST-000005"
std::string MakeFileName(const std::string& dbname, const char* suffix, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%s-%06llu", suffix,
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

// 按 smallest user_key 升序
bool BySmallestKey(const FileMetaData* a, const FileMetaData* b) {
  const int r = a->smallest.user_key().compare(b->smallest.user_key());
  if (r != 0) return r < 0;
  return a->number < b->number;  // 兜底，保证全序
}

// 按编号降序（L0 用：新文件先查）
bool ByNumberDesc(const FileMetaData* a, const FileMetaData* b) {
  return a->number > b->number;
}

}  // namespace

std::string VersionSet::DescriptorFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, "MANIFEST", number);
}

std::string VersionSet::CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

// ---------------------------------------------------------------------------
// Builder：把「一个基线 Version + 若干 VersionEdit」折叠成新 Version
//
// 关键点是**共享 FileMetaData**：新 Version 不复制文件元数据，只是
// refs++。这样几百个文件、上百次版本切换也只是几百次原子加减，
// 而不是几千次字符串拷贝。
// ---------------------------------------------------------------------------
struct VersionSet::Builder {
  VersionSet* vset_;
  Version* base_;
  std::vector<FileMetaData*> added_[kNumLevels];
  std::set<uint64_t> deleted_;

  // base_ 允许为空：Recover 的「全新库」分支里 current_ 还不存在，
  // 而 Builder 的语义就是「从空版本开始折叠」。所以这两处都要判空。
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    if (base_ != nullptr) base_->Ref();
  }

  ~Builder() {
    // 这里必须**无条件**还掉 Builder 自己那一份引用，不能写成
    // `if (f->refs == 1) delete f;`。
    //
    // 那个写法只处理了「没人要」的情况，漏了「有人要」：被新版本采纳的文件
    // 此刻 refs == 2（Builder 一份 + Version 一份），Version 析构时减到 1，
    // 就再也没有人去减第二次——每切换一次版本，就永久漏掉一个 FileMetaData。
    // 症状很隐蔽：程序不崩、测试全绿，只是跑一晚上内存涨几十 MB，
    // 而且 ASan 只会报一串 4400 bytes in 50 objects 的 Direct leak。
    for (int level = 0; level < kNumLevels; ++level) {
      for (FileMetaData* f : added_[level]) {
        assert(f->refs > 0);
        if (--f->refs == 0) delete f;
      }
    }
    if (base_ != nullptr) base_->Unref();
  }

  void Apply(const VersionEdit* edit) {
    for (const auto& p : edit->deleted_files()) {
      deleted_.insert(p.second);
    }
    for (const auto& p : edit->new_files()) {
      FileMetaData* f = new FileMetaData(p.second);
      f->refs = 1;  // Builder 自己持有一份
      added_[p.first].push_back(f);
    }
  }

  void SaveTo(Version* v) {
    for (int level = 0; level < kNumLevels; ++level) {
      std::vector<FileMetaData*> merged;
      const std::vector<FileMetaData*>& base_files = base_->files_[level];

      std::sort(added_[level].begin(), added_[level].end(), BySmallestKey);

      size_t bi = 0;
      size_t ai = 0;
      while (bi < base_files.size() || ai < added_[level].size()) {
        FileMetaData* f = nullptr;
        if (bi >= base_files.size()) {
          f = added_[level][ai++];
        } else if (ai >= added_[level].size()) {
          f = base_files[bi++];
        } else if (BySmallestKey(base_files[bi], added_[level][ai])) {
          f = base_files[bi++];
        } else {
          f = added_[level][ai++];
        }

        if (deleted_.count(f->number) > 0) continue;
        f->refs++;
        merged.push_back(f);
      }

      if (level == 0) {
        // L0 允许重叠，顺序必须是「编号降序」：查的时候先试新文件。
        // 排成升序会让新值被旧值盖住，症状是「更新不生效」。
        std::sort(merged.begin(), merged.end(), ByNumberDesc);
      }
      v->files_[level].swap(merged);
    }
  }
};

VersionSet::VersionSet(const Options& options, const std::string& dbname,
                       TableCache* table_cache)
    : env_(Env::Default()),
      options_(options),
      dbname_(dbname),
      table_cache_(table_cache),
      current_(nullptr) {
  dummy_versions_ = new Version(this);
  // 哨兵自己首尾相接，于是「链表为空」等价于 dummy->next_ == dummy。
  // 插入时直接复用同一段指针操作，不需要为「空链表」写特例。
  dummy_versions_->next_ = dummy_versions_;
  dummy_versions_->prev_ = dummy_versions_;
}

VersionSet::~VersionSet() {
  if (descriptor_log_ != nullptr) {
    delete descriptor_log_;
    descriptor_log_ = nullptr;
  }
  if (descriptor_file_ != nullptr) {
    delete descriptor_file_;
    descriptor_file_ = nullptr;
  }
  if (current_ != nullptr) {
    current_->Unref();
    current_ = nullptr;
  }
  // 哨兵在最后释放：前面所有 Version 都已经从链表上摘掉了。
  // 先把它的两个指针清空——哨兵自己从不在链表上，
  // 否则 ~Version 里的「析构时还在链表上」断言会误报。
  dummy_versions_->next_ = nullptr;
  dummy_versions_->prev_ = nullptr;
  delete dummy_versions_;
  dummy_versions_ = nullptr;
}

void VersionSet::AppendVersion(Version* v) {
  v->Ref();  // current_ 持有一份引用

  {
    std::lock_guard<std::mutex> l(list_mu_);
    v->next_ = dummy_versions_;
    v->prev_ = dummy_versions_->prev_;
    v->prev_->next_ = v;
    dummy_versions_->prev_ = v;
  }

  if (current_ != nullptr) {
    // 把「旧版本有、新版本没有」的文件记到旧版本名下，等它最后一个读者
    // 走掉再删。这就是延迟删除：此刻可能有人正在用旧版本扫表，
    // 立刻删盘会让他的下一次 Next() 拿到 ENOENT。
    std::set<uint64_t> still_live;
    for (int level = 0; level < kNumLevels; ++level) {
      for (FileMetaData* f : v->files_[level]) still_live.insert(f->number);
    }
    for (int level = 0; level < kNumLevels; ++level) {
      for (FileMetaData* f : current_->files_[level]) {
        if (still_live.count(f->number) == 0) {
          current_->files_to_delete_.push_back(f->number);
        }
      }
    }
    current_->Unref();
  }
  current_ = v;
}

void VersionSet::RemoveVersion(Version* v) {
  {
    std::lock_guard<std::mutex> l(list_mu_);
    // 两个指针都要判：next_ 非空只能说明「链过」，prev_ 才是链表的必要条件。
    // 有些 Version 从来没进过链表（Recover 里从零重放的空基线、LogAndApply
    // 失败时丢弃的那个），它们也会 Unref 到 0 走到这里——不能去改邻居。
    if (v->next_ != nullptr && v->prev_ != nullptr) {
      v->next_->prev_ = v->prev_;
      v->prev_->next_ = v->next_;
      v->next_ = nullptr;
      v->prev_ = nullptr;
    }
  }
  delete v;
}

Status VersionSet::ReadCurrentFile(std::string* manifest_name, uint64_t* manifest_number) {
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(CurrentFileName(dbname_), &file);
  if (!s.ok()) return s;

  char buf[512];
  Slice record;
  s = file->Read(sizeof(buf) - 1, &record, buf);
  if (!s.ok()) return s;
  if (record.empty()) return Status::Corruption("CURRENT is empty");

  std::string name(record.data(), record.size());
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
    name.pop_back();
  }

  uint64_t number = 0;
  unsigned long long parsed = 0;
  if (std::sscanf(name.c_str(), "MANIFEST-%llu", &parsed) != 1) {
    return Status::Corruption("CURRENT does not name a MANIFEST: " + name);
  }
  number = static_cast<uint64_t>(parsed);
  *manifest_name = name;
  *manifest_number = number;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// TODO(3.2) VersionSet::Recover
//
// 约束：
//   1. 读 CURRENT 得到 MANIFEST 文件名；不存在说明是全新库 -> 直接 WriteSnapshot
//   2. 打开 MANIFEST，用 log::Reader 循环读记录（**复用第 1 章的 WAL 读端**）
//   3. 每条记录 VersionEdit::DecodeFrom 后交给 Builder::Apply
//   4. 全部应用完 -> Builder::SaveTo 得到新 Version -> AppendVersion
//   5. 校验：version 里的每个 SST 必须真实存在，缺一个就 Corruption
//      （**不要**自动删掉元数据项来「修复」，那等于静默丢数据）
//   6. 用 WriteSnapshot 重写一份紧凑的 MANIFEST（避免无限增长）
//
// 为什么第 6 步值得做：MANIFEST 是增量日志，跑一周可能几十万条。
// 重放它要几百毫秒。重写成一条快照后，下次启动只需要读一条记录。
// 代价是每次启动多写一个文件——几十字节换几百毫秒，很划算。
// ---------------------------------------------------------------------------
Status VersionSet::Recover() {
  // 占位实现：不看 CURRENT、不重放 MANIFEST，直接造一个**空**的版本。
  //
  // 这是「能打开、但看不见文件」的状态，症状很有辨识度：
  //   - DB::Open 不报错
  //   - 打开一个写过十万条数据的库，GetStats().num_sst_files == 0
  //   - 数据只剩 WAL 里还没刷走的那部分 → Recovery.* 还能过，
  //     DbRead.ReopenReadsFromSst 必红
  //   - next_file_number_ 没从 MANIFEST 恢复，从很小的数开始，
  //     写新文件时可能直接覆盖已有的 000005.sst —— 这是真会丢数据的
  //
  // 按上面 6 步实现。第 5 步的校验别改成「跳过缺失文件」，
  // 那看着像修复，实际是静默丢数据。
  Version* empty = new Version(this);
  AppendVersion(empty);
  return WriteSnapshot();
}

Status VersionSet::WriteSnapshot() {
  const uint64_t new_number = NewFileNumber();
  const std::string fname = DescriptorFileName(dbname_, new_number);

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) return s;

  log::Writer writer(file.get());

  VersionEdit snapshot;
  snapshot.SetComparatorName(Slice(icmp_.Name()));
  snapshot.SetLogNumber(log_number_);
  snapshot.SetPrevLogNumber(prev_log_number_);
  snapshot.SetNextFile(next_file_number_);
  snapshot.SetLastSequence(last_sequence_);

  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : current_->files_[level]) {
      snapshot.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  std::string record;
  snapshot.EncodeTo(&record);
  s = writer.AddRecord(record);
  if (s.ok()) s = file->Flush();
  if (s.ok()) s = file->Sync();
  if (!s.ok()) return s;

  // 先让 MANIFEST 落盘，再改 CURRENT —— 顺序反了，崩溃后会指向
  // 一个不存在或不完整的 MANIFEST。
  s = SetCurrentFile(new_number);
  if (!s.ok()) return s;

  if (descriptor_log_ != nullptr) {
    delete descriptor_log_;
    descriptor_log_ = nullptr;
  }
  if (descriptor_file_ != nullptr) {
    delete descriptor_file_;
    descriptor_file_ = nullptr;
  }
  descriptor_file_ = file.release();
  descriptor_log_ = new log::Writer(descriptor_file_);
  manifest_file_number_ = new_number;
  return Status::OK();
}

Status VersionSet::SetCurrentFile(uint64_t descriptor_number) {
  const std::string manifest = DescriptorFileName(dbname_, descriptor_number);
  // CURRENT 里存相对文件名（不带目录），这样整个库目录可以整体搬走
  std::string base = manifest;
  const size_t slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  base.push_back('\n');

  const std::string tmp = dbname_ + "/CURRENT.tmp";
  {
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(tmp, &file);
    if (!s.ok()) return s;
    s = file->Append(Slice(base));
    if (s.ok()) s = file->Flush();
    if (s.ok()) s = file->Sync();
    if (!s.ok()) return s;
  }
  // rename 是原子的：CURRENT 要么指向旧 MANIFEST，要么指向新的，没有中间态
  return env_->RenameFile(tmp, CurrentFileName(dbname_));
}

// ---------------------------------------------------------------------------
// TODO(3.3) VersionSet::LogAndApply
//
// 约束（顺序就是正确性）：
//   1. 构造新 Version（Builder 复制 current_ + 应用 edit）
//   2. **先**把 edit 编码后追加到 MANIFEST 并 Sync
//   3. 落盘成功**之后**才把 current_ 指向新 Version
//   4. 旧 Version Unref
//
// 如果第 2 步失败：内存不动，返回错误。此时磁盘上可能已经写了一半记录，
// 重启后那条记录要么被 CRC 拒绝、要么完整生效（与内存一致），两种情况都安全。
//
// 如果第 3 步之前崩溃：MANIFEST 里有这条 edit，重启重放后生效。
// 内存里那次操作丢了，但那本来就没返回成功给用户。
// ---------------------------------------------------------------------------
Status VersionSet::LogAndApply(VersionEdit* edit, std::mutex* mu) {
  (void)edit;
  (void)mu;
  // 占位实现：不写 MANIFEST、不换 current_，直接说成功。
  //
  // 后果（串起来看才知道有多严重）：
  //   - MANIFEST 永远停在初始快照，重启后文件列表又是空的
  //   - current_ 永远不变，新写的 SST 不被任何 Version 引用
  //   - 于是 DeleteObsoleteFiles 把它们当垃圾删掉 —— 数据真的没了
  // 按上面 4 步写。第 2 步的 Sync 不能省：它是「返回成功 = 已落盘」的
  // 唯一保证，少了它崩溃后会丢一条已经返回成功的 edit。
  return Status::OK();
}

bool VersionSet::NeedsCompaction() const {
  if (current_ == nullptr) return false;
  if (static_cast<int>(current_->files_[0].size()) >= options_.l0_compaction_trigger) {
    return true;
  }
  for (int level = 1; level < kNumLevels - 1; ++level) {
    if (TotalFileSize(current_->files_[level]) > MaxBytesForLevel(options_, level)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// TODO(3.4) VersionSet::PickCompaction
//
// 约束：
//   1. L0 文件数 >= l0_compaction_trigger：输入是**全部** L0 文件（level = 0）
//   2. 否则从 level = 1 往上找第一个超过 MaxBytesForLevel 的层，
//      输入是那一层里**最大的那个文件**（实现简单、效果不差）
//   3. 没有可做的返回 nullptr
//   4. 建好 Compaction 后必须 input_version()->Ref() —— 否则执行期间
//      版本可能被别处 Unref 掉，inputs_ 里的 FileMetaData 就成了悬垂指针
//   5. SetupOtherInputs：算出输入的 key 范围，把下一层重叠的文件加进 inputs_[1]
// ---------------------------------------------------------------------------
Compaction* VersionSet::PickCompaction() {
  // 占位实现：永远说「没什么可压的」。
  //
  // 后果：L0 只增不减，读一次要试 N 个文件（读放大 O(N)）；
  // 覆盖写和墓碑永远清不掉，磁盘只涨不落。
  // Compaction.* / DbRead.FilterKeepsMissesCheap 那组会红。
  //
  // 按上面 5 条写。第 4 条的 input_version()->Ref() 是并发安全的命门：
  // 少这一行，执行 compaction 期间版本被别处 Unref，inputs_ 里的
  // FileMetaData 立刻悬垂，症状是随机的 use-after-free。
  return nullptr;
}

void VersionSet::GetLiveFiles(std::vector<uint64_t>* numbers, uint64_t* total_bytes) const {
  numbers->clear();
  uint64_t total = 0;
  if (current_ == nullptr) {
    if (total_bytes != nullptr) *total_bytes = 0;
    return;
  }
  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : current_->files_[level]) {
      numbers->push_back(f->number);
      total += f->file_size;
    }
  }
  if (total_bytes != nullptr) *total_bytes = total;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  // 沿着双向链表走一圈。dummy_versions_ 是哨兵，它的 next_ 是第一个活着
  // 的 Version，prev_ 是最后一个。链表里所有节点都有 refs_ > 0。
  //
  // 必须持 list_mu_：别的线程可能正在这个链表上摘节点。
  std::lock_guard<std::mutex> l(list_mu_);
  for (Version* v = dummy_versions_->next_; v != dummy_versions_; v = v->next_) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (FileMetaData* f : v->files_[level]) live->insert(f->number);
    }
  }
}

}  // namespace lsm
