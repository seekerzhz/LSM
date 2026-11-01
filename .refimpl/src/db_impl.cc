#include "lsm/db_impl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include "lsm/coding.h"
#include "lsm/compaction_iterator.h"
#include "lsm/fault_env.h"
#include "lsm/log_reader.h"
#include "lsm/merge_iterator.h"
#include "lsm/table_builder.h"
#include "lsm/write_batch.h"

namespace lsm {
namespace {

enum FileType {
  kLogFile,
  kTableFile,
  kTempFile,
  kCurrentFile,
  kDescriptorFile,
  kUnknownFile,
};

// 把目录里的文件名解析成 (编号, 类型)。
// 这个函数是 DeleteObsoleteFiles 的唯一判据——解析错了就会删错文件。
bool ParseFileName(const std::string& name, uint64_t* number, FileType* type) {
  *number = 0;
  *type = kUnknownFile;

  if (name == "CURRENT") {
    *type = kCurrentFile;
    return true;
  }
  if (name == "CURRENT.tmp") {
    *type = kTempFile;
    return true;
  }
  if (name.compare(0, 9, "MANIFEST-") == 0) {
    *type = kDescriptorFile;
    uint64_t n = 0;
    for (size_t i = 9; i < name.size(); ++i) {
      if (name[i] < '0' || name[i] > '9') return false;
      n = n * 10 + static_cast<uint64_t>(name[i] - '0');
    }
    *number = n;
    return true;
  }

  // 形如 000123.log / 000123.sst / 000123.sst.tmp
  if (name.size() < 10) return false;
  if (name.compare(6, 1, ".") != 0) return false;

  uint64_t n = 0;
  for (int i = 0; i < 6; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    n = n * 10 + static_cast<uint64_t>(name[i] - '0');
  }
  *number = n;

  const std::string suffix = name.substr(7);
  if (suffix == "log") {
    *type = kLogFile;
  } else if (suffix == "sst") {
    *type = kTableFile;
  } else if (suffix == "sst.tmp") {
    *type = kTempFile;
  } else {
    return false;
  }
  return true;
}

// 把 WriteBatch 的内容灌进 MemTable
class MemTableInserter : public WriteBatch::Handler {
 public:
  explicit MemTableInserter(MemTable* mem) : mem_(mem) {}

  Status Put(const Slice& key, const Slice& value) override {
    mem_->Add(seq_++, kTypeValue, key, value);
    return Status::OK();
  }
  Status Delete(const Slice& key) override {
    mem_->Add(seq_++, kTypeDeletion, key, Slice());
    return Status::OK();
  }

  SequenceNumber seq_ = 1;

 private:
  MemTable* mem_;
};

}  // namespace

std::string DBImpl::LogFileName(const std::string& dir, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.log", static_cast<unsigned long long>(number));
  return dir + buf;
}

DBImpl::DBImpl(const Options& options) : options_(options) {
  // 故障注入 Env：只有显式打开或设了环境变量时才换。
  // 换成包装层而不是改所有调用点，是因为「让第 N 次写失败」必须能
  // 一网打尽（包括 MANIFEST 写入），否则测试会漏掉关键路径。
  FaultInjectionEnv* fault = nullptr;
  if (options_.use_fault_env) {
    fault = FaultInjectionEnv::CreateFromEnv(Env::Default());
    if (fault == nullptr) {
      // 打开了开关但没配参数：给一个默认的「第 3 次 append 失败」
      fault = new FaultInjectionEnv(Env::Default());
      fault->SetFault(3, "append", false);
    }
  }
  if (fault != nullptr) {
    owned_env_.reset(fault);
    env_ = fault;
  } else {
    env_ = Env::Default();
  }
}

DBImpl::~DBImpl() {
  // 用户忘了 Close 也不能泄漏线程和 fd
  Close();
}

Status DBImpl::Init() {
  Status s = env_->CreateDir(options_.db_path);
  if (!s.ok()) return s;

  table_cache_.reset(new TableCache(options_, options_.db_path));
  versions_.reset(new VersionSet(options_, options_.db_path, table_cache_.get()));

  s = versions_->Recover();
  if (!s.ok()) return s;

  s = Recover();
  if (!s.ok()) return s;

  bg_thread_ = std::thread(&DBImpl::BackgroundLoop, this);
  return Status::OK();
}

Status DBImpl::ReplayLog(uint64_t log_number, SequenceNumber* max_seq) {
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(LogFileName(options_.db_path, log_number), &file);
  if (!s.ok()) return s;

  log::Reader reader(file.get(), nullptr);
  std::string scratch;
  Slice record;
  WriteBatch batch;
  MemTableInserter inserter(mem_);

  while (reader.ReadRecord(&record, &scratch)) {
    // 一条 WriteBatch 至少 12 字节（seq 8 + count 4）
    if (record.size() < 12) continue;
    s = batch.SetContents(record);
    if (!s.ok()) return s;
    inserter.seq_ = batch.Sequence();
    s = batch.Iterate(&inserter);
    if (!s.ok()) return s;
    const SequenceNumber last = batch.Sequence() + static_cast<SequenceNumber>(batch.Count()) - 1;
    if (last > *max_seq) *max_seq = last;
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 恢复：清理临时文件 -> 重放 WAL -> 开新 WAL -> 记进 MANIFEST
//
// 顺序很重要：**先把旧 WAL 都重放完，再创建新 WAL**。
// 反过来会让新 WAL 的编号小于未重放的旧日志，Recover 时就分不清
// 「哪些日志还没进 SST」。
// ---------------------------------------------------------------------------
Status DBImpl::Recover() {
  std::vector<std::string> names;
  Status s = env_->ListDir(options_.db_path, &names);
  if (!s.ok()) return s;

  std::vector<uint64_t> log_numbers;
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type = kUnknownFile;
    if (!ParseFileName(name, &number, &type)) continue;

    if (type == kTempFile) {
      // 崩溃残留：.tmp 意味着「写完之前就挂了」，内容不完整且没进 MANIFEST
      env_->RemoveFile(options_.db_path + "/" + name);
      continue;
    }
    if (type == kLogFile) log_numbers.push_back(number);
    if (type == kTableFile) versions_->MarkFileNumberUsed(number);
    if (type == kDescriptorFile) versions_->MarkFileNumberUsed(number);
  }
  std::sort(log_numbers.begin(), log_numbers.end());

  mem_ = new MemTable(versions_->icmp());
  mem_->Ref();

  // 只重放「版本里记录的 log_number 及之后」的日志。
  // 更早的日志内容已经在 SST 里了，再放一遍会产生重复记录
  // （虽然 seq 不同不会错，但会白白撑大内存）。
  const uint64_t min_log = versions_->log_number();
  SequenceNumber max_seq = 0;
  for (uint64_t number : log_numbers) {
    if (number < min_log) continue;
    s = ReplayLog(number, &max_seq);
    if (!s.ok()) return s;
  }

  const SequenceNumber observed = std::max(max_seq, versions_->LastSequence());
  versions_->SetLastSequence(observed);
  last_sequence_.store(observed + 1);

  // 新 WAL
  log_number_ = versions_->NewFileNumber();
  const std::string fname = LogFileName(options_.db_path, log_number_);
  std::unique_ptr<WritableFile> lfile;
  s = env_->NewWritableFile(fname, &lfile);
  if (!s.ok()) return s;
  logfile_ = std::move(lfile);
  log_.reset(new log::Writer(logfile_.get()));

  VersionEdit edit;
  edit.SetLogNumber(log_number_);
  edit.SetNextFile(versions_->next_file_number());
  edit.SetLastSequence(observed);
  s = versions_->LogAndApply(&edit, &mu_);
  if (!s.ok()) return s;

  // 把重放期间产生的「更早的日志」删掉
  for (uint64_t number : log_numbers) {
    if (number < log_number_) {
      env_->RemoveFile(LogFileName(options_.db_path, number));
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// TODO(4.1) Write：并发写入
//
// 约束：
//   1. 持 mu_：检查 bg_error_、分配 seq、写 WAL、写 MemTable
//   2. 分配 seq 用 last_sequence_.fetch_add(Count())，无锁，且天然不重复
//   3. WAL 写失败必须**立即返回**，内存不能改（否则重启后数据消失，
//      但用户在内存里还能读到，这叫「假成功」）
//   4. opt.sync 时 Sync()，返回成功才意味着断电不丢
//   5. MemTable 超过阈值时切新表（SwitchMemTableLocked）
//   6. 解锁之后再 MaybeScheduleCompaction()——它自己要拿锁
// ---------------------------------------------------------------------------
Status DBImpl::Write(const WriteOptions& opt, WriteBatch* batch) {
  std::unique_lock<std::mutex> l(mu_);
  if (closed_) return Status::IOError("database is closed");
  if (!bg_error_.ok()) return bg_error_;

  // write stall：L0 文件太多，先等 compaction 把文件数降下来。
  // 不这么做，写入速度超过 compaction 速度时 L0 会无限增长，
  // 每次 Get 要检查的 filter 数跟着涨，读延迟雪崩。
  if (versions_->current() != nullptr &&
      static_cast<int>(versions_->current()->files_[0].size()) >= options_.l0_stop_writes_trigger) {
    stat_write_stalls_.fetch_add(1, std::memory_order_relaxed);
    if (!bg_scheduled_ && !shutting_down_) {
      bg_scheduled_ = true;
      bg_cv_.notify_one();
    }
    bg_finished_cv_.wait_for(l, std::chrono::seconds(10), [this] {
      return shutting_down_ ||
             static_cast<int>(versions_->current()->files_[0].size()) <
                 options_.l0_stop_writes_trigger;
    });
    if (shutting_down_) return Status::IOError("shutting down");
    if (!bg_error_.ok()) return bg_error_;
  }

  const SequenceNumber seq = last_sequence_.fetch_add(batch->Count());
  batch->SetSequence(seq);

  Status s = log_->AddRecord(batch->Repr());
  if (s.ok() && opt.sync) s = logfile_->Sync();
  if (!s.ok()) {
    // 内存未动。用户收到错误，磁盘上可能有半条记录——重启时会被 CRC 拒绝。
    return s;
  }

  MemTableInserter inserter(mem_);
  inserter.seq_ = seq;
  s = batch->Iterate(&inserter);
  if (!s.ok()) return s;

  if (mem_->ApproximateMemoryUsage() > options_.write_buffer_size) {
    // 不能直接 SwitchMemTableLocked：如果上一个 imm_ 还没落盘，
    // 它里面没有位置放新的 imm。这里的处理是「当场把它刷掉」——
    // 也就是同步写一次 SST。真实系统（LevelDB）会让写线程在 cv 上等
    // 后台线程做完，效果一样但不会让这个写线程干脏活。
    if (imm_ == nullptr) {
      s = SwitchMemTableLocked();
      if (!s.ok()) return s;
    } else {
      l.unlock();
      s = FlushImm();
      l.lock();
      if (!s.ok()) return s;
      if (imm_ == nullptr && !shutting_down_ && !closed_) {
        s = SwitchMemTableLocked();
        if (!s.ok()) return s;
      }
    }
  }

  l.unlock();
  MaybeScheduleCompaction();
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(o, &batch);
}

Status DBImpl::Delete(const WriteOptions& o, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(o, &batch);
}

// ---------------------------------------------------------------------------
// TODO(4.2) Get：无锁读路径
//
// 约束（每一条都对应一个真实 bug）：
//   1. 临界区只做「拷指针 + Ref」，**绝不 IO**
//   2. 顺序：mem_ -> imm_ -> Version
//   3. Ref 必须在锁内（锁外 Ref 时对象可能已被别人 Unref 到 0）
//   4. Unref 必须在所有访问之后，顺序与 Ref 相反
//   5. 命中墓碑就停，不要往下找——否则删除会复活
// ---------------------------------------------------------------------------
Status DBImpl::Get(const ReadOptions& opt, const Slice& key, std::string* value) {
  const SequenceNumber snapshot =
      opt.snapshot > 0 ? opt.snapshot : last_sequence_.load(std::memory_order_relaxed) - 1;
  LookupKey lkey(key, snapshot);

  MemTable* mem = nullptr;
  MemTable* imm = nullptr;
  Version* current = nullptr;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return Status::IOError("database is closed");
    mem = mem_;
    mem->Ref();
    imm = imm_;
    if (imm != nullptr) imm->Ref();
    current = versions_->current();
    if (current != nullptr) current->Ref();
  }

  Status s;
  bool found = mem->Get(lkey, value, &s);
  if (!found && imm != nullptr) {
    found = imm->Get(lkey, value, &s);
  }
  if (!found && current != nullptr) {
    // 这里必须写 `struct GetStats`：裸写 GetStats 会被解析成 DBImpl::GetStats()
    // 这个成员函数（名字被成员函数遮蔽了）。elaborated-type-specifier 能消除歧义。
    struct GetStats stats;
    s = current->Get(opt, lkey, value, &stats);
    found = s.ok();
    stat_blocks_read_.fetch_add(stats.blocks_read, std::memory_order_relaxed);
    stat_filter_checks_.fetch_add(stats.filter_checks, std::memory_order_relaxed);
  }

  mem->Unref();
  if (imm != nullptr) imm->Unref();
  if (current != nullptr) current->Unref();

  stat_gets_.fetch_add(1, std::memory_order_relaxed);

  if (!found) {
    return s.ok() ? Status::NotFound("key not found") : s;
  }
  return s;
}

// ---------------------------------------------------------------------------
// TODO(2.12) SwitchMemTableLocked：mem_ -> imm_，换新 WAL
//
// 约束（顺序不能错，这是崩溃安全的根基）：
//   1. 先 Sync 旧 WAL：imm_ 里的数据必须已经在盘上，否则 flush 到一半
//      崩溃时那段数据两头都没有
//   2. imm_ = mem_；新建 mem_
//   3. 分配新 WAL 编号、打开新文件、**立刻**把新的 log_number 记进 MANIFEST
//      第 3 步和第 2 步之间崩溃 -> 重启时按旧 log_number 重放，数据不丢
// ---------------------------------------------------------------------------
Status DBImpl::SwitchMemTableLocked() {
  if (imm_ != nullptr) {
    // 上一个 imm 还没落盘。让调用方（Write）等后台线程做完。
    return Status::IOError("memtable switch while previous flush is pending");
  }

  Status s = logfile_->Sync();
  if (!s.ok()) return s;

  imm_ = mem_;
  imm_wal_number_ = log_number_;

  mem_ = new MemTable(versions_->icmp());
  mem_->Ref();

  log_number_ = versions_->NewFileNumber();
  const std::string fname = LogFileName(options_.db_path, log_number_);
  std::unique_ptr<WritableFile> file;
  s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) return s;
  logfile_ = std::move(file);
  log_.reset(new log::Writer(logfile_.get()));

  VersionEdit edit;
  edit.SetLogNumber(log_number_);
  edit.SetLastSequence(last_sequence_.load(std::memory_order_relaxed) - 1);
  // 已持锁，传 nullptr 避免自死锁
  return versions_->LogAndApply(&edit, nullptr);
}

// ---------------------------------------------------------------------------
// TODO(2.12 续) FlushImm：把 imm_ 写成 SST
//
// 约束（顺序不能错）：
//   1. 写 %06d.sst.tmp
//   2. Sync（不 sync 就 rename，崩溃后会得到一个「名字对但内容缺」的文件）
//   3. Rename 到正式名（原子：崩溃时只有「.tmp 存在」或「正式名存在」）
//   4. LogAndApply 把新文件加进 L0、更新 log_number
//   5. 释放 imm_，删旧 WAL
//
// 全程**不持 mu_**：写盘可能要几十毫秒，持锁会让所有写线程停等。
// ---------------------------------------------------------------------------
Status DBImpl::FlushImm() {
  // 同一时刻只允许一个线程把 imm_ 落盘。前台 Flush() 和后台线程都会调进来，
  // 没有这把锁会出现「两个线程写同一个 imm_」：产出两份重复 SST，
  // 而且先完成的那方把 imm_ 置空后，另一方的 `imm_->Unref()` 直接解引用空指针。
  std::lock_guard<std::mutex> flush_guard(flush_mu_);

  MemTable* imm = nullptr;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (imm_ == nullptr) return Status::OK();
    imm = imm_;
    imm->Ref();
  }

  uint64_t file_number = 0;
  {
    std::lock_guard<std::mutex> l(mu_);
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
  }

  const std::string final_name = TableCache::FileName(options_.db_path, file_number);
  const std::string tmp_name = final_name + ".tmp";

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(tmp_name, &file);

  uint64_t file_size = 0;
  InternalKey smallest;
  InternalKey largest;
  uint64_t count = 0;
  SequenceNumber max_seq = 0;

  if (s.ok()) {
    TableBuilder builder(options_, file.get());
    std::unique_ptr<Iterator> iter(imm->NewIterator());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (count == 0) smallest.DecodeFrom(iter->key());
      largest.DecodeFrom(iter->key());
      ParsedInternalKey p;
      if (ParseInternalKey(iter->key(), &p) && p.sequence > max_seq) max_seq = p.sequence;
      builder.Add(iter->key(), iter->value());
      ++count;
    }
    if (iter->status().ok()) {
      s = builder.Finish();
    } else {
      s = iter->status();
    }
    if (s.ok()) s = file->Sync();
    if (s.ok()) file_size = builder.FileSize();
    if (!s.ok()) builder.Abandon();
  }
  file.reset();  // 关 fd，否则 rename 之后旧句柄还指着旧 inode

  if (!s.ok()) {
    env_->RemoveFile(tmp_name);
    {
      std::lock_guard<std::mutex> l(mu_);
      pending_outputs_.erase(file_number);
      imm_->Unref();
      imm_ = nullptr;
    }
    imm->Unref();
    return s;
  }

  if (count == 0) {
    // 空的 MemTable：不产生文件，也要把 imm_ 清掉
    env_->RemoveFile(tmp_name);
    {
      std::lock_guard<std::mutex> l(mu_);
      pending_outputs_.erase(file_number);
      imm_->Unref();
      imm_ = nullptr;
      stat_flushes_.fetch_add(1, std::memory_order_relaxed);
    }
    imm->Unref();
    DeleteObsoleteFiles();
    return Status::OK();
  }

  s = env_->RenameFile(tmp_name, final_name);
  if (!s.ok()) {
    env_->RemoveFile(tmp_name);
    {
      std::lock_guard<std::mutex> l(mu_);
      pending_outputs_.erase(file_number);
      imm_->Unref();
      imm_ = nullptr;
    }
    imm->Unref();
    return s;
  }

  VersionEdit edit;
  edit.AddFile(0, file_number, file_size, smallest, largest);
  edit.SetLastSequence(max_seq);
  edit.SetLogNumber(log_number_);
  s = versions_->LogAndApply(&edit, &mu_);

  {
    std::lock_guard<std::mutex> l(mu_);
    pending_outputs_.erase(file_number);
    imm_->Unref();
    imm_ = nullptr;
    stat_flushes_.fetch_add(1, std::memory_order_relaxed);
  }
  imm->Unref();

  DeleteObsoleteFiles();
  return s;
}

Status DBImpl::Flush() {
  {
    std::lock_guard<std::mutex> l(mu_);
    // mem_ 为空说明 Init 没走完（DB::Open 失败后 delete impl 会走到这里）。
    // 少了这个判断，析构路径会对着 nullptr 调 ApproximateMemoryUsage()。
    if (mem_ == nullptr) return Status::OK();
    if (imm_ == nullptr && mem_->ApproximateMemoryUsage() == 0) return Status::OK();
    if (imm_ == nullptr) {
      Status s = SwitchMemTableLocked();
      if (!s.ok()) return s;
    }
  }
  return FlushImm();
}

// ---------------------------------------------------------------------------
// TODO(3.6) DoCompactionWork
//
// 约束（顺序是本课程最需要背的部分）：
//   1. 为所有输入文件各建一个迭代器，MergingIterator 归并
//   2. CompactionIterator 包一层，丢掉被覆盖的版本和最底层的墓碑
//   3. 输出文件超过 max_file_size 就切下一个——**切点必须在 user_key 变深处**
//      否则同一个 key 的多个版本会被分到两个文件，第二个文件的 smallest
//      就无法代表「这个 key 从这里开始」
//   4. 每个输出文件：写 .tmp -> Sync -> Rename
//   5. VersionEdit：DeleteFile 所有输入 + AddFile 所有输出
//   6. LogAndApply
// ---------------------------------------------------------------------------
Status DBImpl::DoCompactionWork(Compaction* c) {
  std::vector<Iterator*> children;
  for (int which = 0; which < 2; ++which) {
    for (FileMetaData* f : c->inputs(which)) {
      children.push_back(table_cache_->NewIterator(f->number, f->file_size));
    }
  }
  if (children.empty()) return Status::OK();

  MergingIterator merge(&versions_->icmp(), &children);
  CompactionIterator iter(&merge, &versions_->icmp(), c);

  VersionEdit edit;
  c->AddInputDeletions(&edit);

  // 当前输出文件的状态
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  std::string out_tmp_name;
  std::string out_final_name;
  uint64_t out_number = 0;
  InternalKey out_smallest;
  InternalKey out_largest;
  bool out_has_entries = false;
  // 上一个 key 的 user_key：用来判断「能不能在这里切文件」
  std::string last_user_key;

  auto finish_output = [&]() -> Status {
    if (!builder) return Status::OK();
    Status s = builder->Finish();
    const uint64_t size = builder->FileSize();
    if (s.ok()) s = outfile->Sync();
    if (!s.ok()) {
      builder->Abandon();
      builder.reset();
      outfile.reset();
      env_->RemoveFile(out_tmp_name);
      return s;
    }
    builder.reset();
    outfile.reset();
    s = env_->RenameFile(out_tmp_name, out_final_name);
    if (!s.ok()) return s;
    edit.AddFile(c->level() + 1, out_number, size, out_smallest, out_largest);
    return Status::OK();
  };

  auto start_output = [&]() -> Status {
    // 编号必须在锁内分配。NewFileNumber() 只是 `next_file_number_++`，
    // 而 compaction 全程不持 mu_ —— 和 FlushImm 撞在一起就会拿到**同一个编号**，
    // 后写的文件把先写的覆盖掉：症状是「偶发地丢几十条记录」，
    // 而且两个文件都存在于版本里，怎么看都像数据自己消失了。
    {
      std::lock_guard<std::mutex> l(mu_);
      out_number = versions_->NewFileNumber();
      pending_outputs_.insert(out_number);
    }
    out_final_name = TableCache::FileName(options_.db_path, out_number);
    out_tmp_name = out_final_name + ".tmp";
    std::unique_ptr<WritableFile> f;
    Status s = env_->NewWritableFile(out_tmp_name, &f);
    if (!s.ok()) return s;
    outfile = std::move(f);
    builder.reset(new TableBuilder(options_, outfile.get()));
    out_has_entries = false;
    return Status::OK();
  };

  Status s = start_output();

  for (iter.SeekToFirst(); s.ok() && iter.Valid(); iter.Next()) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(iter.key(), &ikey)) {
      s = Status::Corruption("bad internal key during compaction");
      break;
    }

    const std::string user_key = ikey.user_key.ToString();
    const bool new_user_key = (user_key != last_user_key);

    // 只有「换了一个 user_key」时才允许切文件
    if (builder && out_has_entries && builder->FileSize() >= c->MaxOutputFileSize() &&
        new_user_key) {
      s = finish_output();
      if (!s.ok()) break;
      s = start_output();
      if (!s.ok()) break;
    }

    if (!out_has_entries) {
      out_smallest.DecodeFrom(iter.key());
      out_has_entries = true;
    }
    out_largest.DecodeFrom(iter.key());
    last_user_key = user_key;
    builder->Add(iter.key(), iter.value());
  }

  if (s.ok()) s = iter.status();
  if (!s.ok()) {
    if (builder) {
      builder->Abandon();
      builder.reset();
      outfile.reset();
      env_->RemoveFile(out_tmp_name);
    }
    return s;
  }

  if (out_has_entries) {
    s = finish_output();
  } else if (builder) {
    // 所有记录都被丢弃了：不留空文件
    builder->Abandon();
    builder.reset();
    outfile.reset();
    env_->RemoveFile(out_tmp_name);
  }
  if (!s.ok()) return s;

  s = versions_->LogAndApply(&edit, &mu_);
  if (s.ok()) stat_compactions_.fetch_add(1, std::memory_order_relaxed);
  DeleteObsoleteFiles();
  return s;
}

Status DBImpl::CompactOnce() {
  // 同一时刻只允许一个 compaction 在跑，见 compaction_mu_ 的注释
  std::lock_guard<std::mutex> compaction_guard(compaction_mu_);

  Compaction* c = nullptr;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (shutting_down_ || closed_) return Status::IOError("database is closed");
    c = versions_->PickCompaction();
  }
  if (c == nullptr) return Status::OK();

  Status s = DoCompactionWork(c);
  c->input_version()->Unref();
  delete c;
  return s;
}

// ---------------------------------------------------------------------------
// 删除不再被任何活版本引用的文件
//
// 判据是「活版本集合」，不是「current()」：一个正在扫表的读者可能还拿着
// 旧 Version，它引用的文件不能删。
// ---------------------------------------------------------------------------
void DBImpl::DeleteObsoleteFiles() {
  // 全程持锁，**不能**「先收集活文件集合、解锁、再按这个集合删」。
  // 中间那段窗口里，另一个人（compaction / flush）可能刚刚 LogAndApply
  // 把一个新 SST 加进版本：文件已经在盘上，但我们的 live 集合里没有它，
  // 于是刚写出来的数据被当成垃圾删掉。症状是「压缩以后部分 key 消失」，
  // 而且只在有并发写的时候出现。
  std::lock_guard<std::mutex> l(mu_);
  if (versions_ == nullptr) return;

  std::set<uint64_t> live;
  versions_->AddLiveFiles(&live);
  live.insert(log_number_);
  if (imm_ != nullptr) live.insert(imm_wal_number_);
  const uint64_t min_log = log_number_;

  std::vector<std::string> names;
  if (!env_->ListDir(options_.db_path, &names).ok()) return;

  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type = kUnknownFile;
    if (!ParseFileName(name, &number, &type)) continue;

    const std::string path = options_.db_path + "/" + name;
    if (type == kLogFile && number < min_log) {
      // 这份日志的内容已经全部进 SST 了
      env_->RemoveFile(path);
    } else if (type == kTableFile && live.count(number) == 0 &&
               pending_outputs_.count(number) == 0) {
      // 先踢缓存再删：fd 还开着的时候删文件，空间不会立刻回收
      table_cache_->Evict(number);
      env_->RemoveFile(path);
    }
    // 注意：这里**不删 .tmp**。
    // .tmp 有两种含义：「崩溃留下的半成品」和「此刻正在被另一个线程写」。
    // 后台线程 flush 与用户线程 flush 可能同时进行，看到别人的 .tmp 就删，
    // 对方 rename 时会拿到 ENOENT —— 症状是「偶发的一次 flush 失败，
    // 数据还留在 WAL 里，但内存表已经换掉了」。
    // 真正的崩溃残留由 Recover() 在单线程阶段统一清掉。
  }
}

// ---------------------------------------------------------------------------
// TODO(4.3) MaybeScheduleCompaction
//
// 约束：
//   1. 全程持 mu_
//   2. 条件：bg_error_ 为 OK、未关闭、尚未调度、且（imm_ 存在 或 需要 compaction）
//   3. 满足则 bg_scheduled_ = true + notify_one
//   4. **不在这里起线程**：线程在 Init 时起一次，长期存活
//
// 「检查 + 设置」必须在同一把锁内。否则两个写线程同时看到
// bg_scheduled_ == false，都会去调度（这里无害，但换成「起线程」就会起两个）。
// ---------------------------------------------------------------------------
void DBImpl::MaybeScheduleCompaction() {
  std::lock_guard<std::mutex> l(mu_);
  if (!bg_error_.ok()) return;
  if (shutting_down_ || closed_) return;
  if (bg_scheduled_) return;
  if (imm_ == nullptr && !versions_->NeedsCompaction()) return;
  bg_scheduled_ = true;
  bg_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// TODO(4.4) BackgroundLoop
//
// 骨架：
//   while (true) {
//     wait 直到 bg_scheduled_ 或 shutting_down_
//     if (shutting_down_) break;
//     bg_scheduled_ = false;
//     解锁 -> BackgroundCompaction() -> 加锁
//     bg_finished_cv_.notify_all();
//   }
//
// 三个坑：
//   - wait 必须带谓词（虚假唤醒会让它在没工作时空转）
//   - BackgroundCompaction 必须在**锁外**跑（否则 fsync 期间前台全卡住）
//   - 结束前如果 imm_ 还在，做最后一次 flush（否则 Close 之后数据只在 WAL 里）
// ---------------------------------------------------------------------------
void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mu_);
  while (true) {
    while (!bg_scheduled_ && !shutting_down_) {
      bg_cv_.wait(l, [this] { return bg_scheduled_ || shutting_down_; });
    }
    if (shutting_down_) break;

    bg_scheduled_ = false;
    l.unlock();
    BackgroundCompaction();
    l.lock();
    bg_finished_cv_.notify_all();
  }

  // 退出前把 imm_ 落盘。不这么做，最后一次切换之后的数据就只在 WAL 里，
  // 虽然重启能恢复，但 Close 的语义应该是「数据已经安全」。
  //
  // 注意：此刻 `l` 还持着 mu_，所以这里直接读 imm_ 是安全的。
  // 千万别在这里再套一个 std::lock_guard(mu_) —— std::mutex 不可重入，
  // 那是一个必定触发的自死锁，表现是 Close 卡在 join 上。
  const bool has_imm = (imm_ != nullptr);
  l.unlock();
  if (has_imm) FlushImm();
  l.lock();
  bg_finished_cv_.notify_all();
}

void DBImpl::BackgroundCompaction() {
  // 先 flush：imm_ 占着内存，越早落盘越好
  {
    std::lock_guard<std::mutex> l(mu_);
    if (imm_ != nullptr) {
      // 记下「有活要干」，flush 结束后继续
    }
  }

  Status s = FlushImm();
  if (!s.ok()) {
    std::lock_guard<std::mutex> l(mu_);
    bg_error_ = s;
    return;
  }

  // 再做 compaction。一次只做一轮：做完再检查是否还需要，
  // 避免长时间霸占后台线程（Close 时要能及时响应）。
  s = CompactOnce();
  if (!s.ok() && !s.IsNotFound()) {
    std::lock_guard<std::mutex> l(mu_);
    // 记下来但**不退出循环**：可能只是这一次失败（磁盘满），
    // 退出循环会让引擎永久失去自愈能力。
    bg_error_ = s;
  }
}

Stats DBImpl::GetStats() {
  Stats st;
  st.flushes = stat_flushes_.load(std::memory_order_relaxed);
  st.compactions = stat_compactions_.load(std::memory_order_relaxed);
  st.write_stalls = stat_write_stalls_.load(std::memory_order_relaxed);
  st.gets = stat_gets_.load(std::memory_order_relaxed);
  st.blocks_read = stat_blocks_read_.load(std::memory_order_relaxed);
  st.filter_checks = stat_filter_checks_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> l(mu_);
  if (versions_ != nullptr && versions_->current() != nullptr) {
    std::vector<uint64_t> nums;
    uint64_t bytes = 0;
    versions_->GetLiveFiles(&nums, &bytes);
    st.num_sst_files = static_cast<int>(nums.size());
    st.disk_usage = bytes;
    for (int level = 0; level < kNumLevels; ++level) {
      st.level_files[level] = versions_->current()->NumFiles(level);
    }
  }
  return st;
}

// ---------------------------------------------------------------------------
// TODO(4.5) Close
//
// 顺序不可调换：
//   1. 持锁设 shutting_down_ + notify_all，解锁
//   2. join 后台线程（**必须在释放任何成员之前**）
//   3. flush 还没落盘的 MemTable
//   4. Sync WAL，释放 log / logfile
//   5. 释放 mem_ / imm_
//   6. 释放 versions_（它 Unref 最后一个 Version，可能触发删文件）
// ---------------------------------------------------------------------------
Status DBImpl::Close() {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return Status::OK();
    closed_ = true;
    shutting_down_ = true;
    bg_cv_.notify_all();
  }

  // 第 2 步必须在所有 delete 之前：否则后台线程醒来会访问已释放的 versions_
  if (bg_thread_.joinable()) bg_thread_.join();

  // 后台线程退出时已经 flush 过 imm_；这里兜底一次
  Status s = Flush();
  if (!s.ok() && s.IsIOError() == false) {
    // flush 失败不是致命问题：数据还在 WAL 里，重启会重放
  }

  {
    std::lock_guard<std::mutex> l(mu_);
    if (logfile_ != nullptr) {
      logfile_->Sync();  // 尽力而为
    }
    log_.reset();
    logfile_.reset();
    if (mem_ != nullptr) {
      mem_->Unref();
      mem_ = nullptr;
    }
    if (imm_ != nullptr) {
      imm_->Unref();
      imm_ = nullptr;
    }
  }

  table_cache_.reset();
  versions_.reset();
  return Status::OK();
}

Status DB::Open(const Options& options, DB** dbptr) {
  *dbptr = nullptr;
  DBImpl* impl = new DBImpl(options);
  Status s = impl->Init();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *dbptr = impl;
  return Status::OK();
}

}  // namespace lsm
