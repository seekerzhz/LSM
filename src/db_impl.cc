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
  (void)opt;
  // 占位实现：最朴素的一条路径 —— 加锁、分配 seq、写 WAL、写 MemTable。
  //
  // 它「能用」，但缺了 TODO 4.1 的四件事，每一件都对应一类真实故障：
  //   1. write stall：L0 文件太多时不停下来等 compaction，写入会一直跑在
  //      compaction 前面，L0 无限增长，每次 Get 要试的文件数跟着涨
  //   2. opt.sync 时的 Sync()：不 sync，「返回成功」不等于「已落盘」，
  //      断电会丢掉最后一批已经告诉用户成功的写
  //   3. MemTable 超阈值切表：不切表就永远没有 imm_，也就永远没有 SST
  //      （做完前三个 Lab 你会发现 num_sst_files 一直是 0）
  //   4. 解锁后 MaybeScheduleCompaction：后台线程永远不被叫醒
  //
  // 症状很典型：单线程、小数据量的用例照样过；Concurrency.* / Shutdown.* 红。
  std::lock_guard<std::mutex> l(mu_);
  if (closed_) return Status::IOError("database is closed");
  if (!bg_error_.ok()) return bg_error_;

  const SequenceNumber seq = last_sequence_.fetch_add(batch->Count());
  batch->SetSequence(seq);

  Status s = log_->AddRecord(batch->Repr());
  if (!s.ok()) {
    // 内存未动。用户收到错误，磁盘上可能有半条记录——重启时会被 CRC 拒绝。
    return s;
  }

  MemTableInserter inserter(mem_);
  inserter.seq_ = seq;
  return batch->Iterate(&inserter);
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
  // 占位实现：查找顺序是对的（mem_ -> imm_ -> Version），但**没有引用计数保护**。
  //
  // 缺的就是 TODO 4.2 的全部内容：临界区里拷指针的同时 `Ref()`，
  // 访问完之后按相反顺序 `Unref()`。
  //
  // 症状特别有欺骗性：单线程跑所有用例都过，因为没人跟你抢；
  // 一旦后台 compaction 同时跑，`current` 指向的 Version 可能在这三行
  // 之间被别处 Unref 到 0 并释放 —— use-after-free，崩溃点离根因几千行远。
  // 打开 TSan / ASan 才有确定性证据。
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
    imm = imm_;
    current = versions_->current();
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
  // 占位实现：不切换、不换 WAL，直接说成功。
  //
  // 后果（这就是骨架的「可运行但功能不全」状态）：
  //   - mem_ 永远不变成 imm_，所以永远没有东西可以 flush 成 SST
  //   - 所有写入都留在当前 MemTable 和同一个 WAL 里
  //   - 重启后数据还在（Recover 会重放 WAL），但 GetStats().num_sst_files 恒为 0
  //
  // 也就是说：Lab 0/1 的用例照样能过，一碰 SST / compaction 就红。
  // 按上面 3 步写；第 1 步（先 Sync 旧 WAL）漏掉的话，
  // 崩溃恢复测试会在「kill -9 之后少了最后几条」上暴露。
  return Status::OK();
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
  // 占位实现：什么都不做。
  //
  // 配合上面 SwitchMemTableLocked 的占位，imm_ 永远是 nullptr，
  // 这里直接返回 OK 就是自洽的。等你实现完上面那个，这里才会真的被调到。
  //
  // 按上面 5 步写。三个最容易漏的点：
  //   - 忘了 file->Sync() 就 Rename：崩溃后名字对、内容缺
  //   - file.reset() 忘写：rename 之后旧 fd 还指着旧 inode
  //   - 全程持 mu_：写盘几十毫秒，所有写线程一起停等
  return Status::OK();
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
  (void)c;
  // 占位实现：认领了任务，但什么也没做 —— 不归并、不写输出文件、
  // 也不删输入文件。
  //
  // 后果很微妙：数据一点没丢（输入文件还在原位，版本也没动），
  // 但 L0 永远不会被压空。所以：
  //   - Compaction.L0FilesMergeIntoBaseLevel 会红（L0 还是满的）
  //   - Compaction.KeepsEveryLiveKey 反而会过（什么都没删，当然不丢）
  // 这种「只红一半」是有意的：它让你能区分「功能没做」和「做错了」。
  //
  // 按上面 6 步写。第 3 步的切点最容易写错：切点必须在 user_key 变深处，
  // 在同一个 key 的版本中间切，第二个文件的 smallest 就不代表这个 key 的起点。
  return Status::OK();
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
  // 占位实现：什么都不做，后台线程永远不会被叫醒。
  //
  // 后果：imm_ 没人落盘（除非 Write 里那条同步路径走到），
  // compaction 永不发生，L0 只增不减。
  // 单线程的用例只要手动调过 Flush() 就照过，Concurrency.* 会红。
  //
  // 按上面 4 条写。「检查 bg_scheduled_」和「置位 bg_scheduled_」必须在
  // 同一把锁内，否则两个写线程同时看到 false，各自去调度一次。
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
  // 占位实现：线程起来就退出。
  //
  // 为什么不是别的降级形态：
  //   - 空转：白烧一个核，Close 还要等它醒
  //   - 一直 wait 而不判 shutting_down_：Close 的 join 会永远卡住
  //   - 直接 return：既不烧 CPU 也不卡死，唯一安全的骨架形态
  //
  // 后果：后台 compaction 永远不发生。再叠加上面 MaybeScheduleCompaction
  // 的占位，整条后台路径都是死的 —— 但前台读写一切正常。
  //
  // 按上面骨架写。三个坑都在注释里，最容易漏的是「退出前那次 flush」
  // 和「不要在已持锁的作用域里再 lock 一次」（std::mutex 不可重入，
  // 那是必定触发的自死锁，表现是 Close 卡在 join 上）。
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

  // 这两句**保留**。不 join 的话 ~DBImpl 里那个 std::thread 还是 joinable，
  // 析构会直接 std::terminate —— 那是「骨架跑不起来」，不是「功能没实现」，
  // 对排查没有任何帮助。
  if (bg_thread_.joinable()) bg_thread_.join();

  // 占位实现：join 之后的四步一件都没做。
  //
  // 缺的是 TODO 4.5 的第 3–6 步：
  //   3. flush 还没落盘的 MemTable（后台线程退出时那一次兜底）
  //   4. Sync WAL，再释放 log / logfile
  //   5. 释放 mem_ / imm_
  //   6. 释放 versions_ —— 它会 Unref 最后一个 Version，可能触发删文件
  //
  // 后果：Close 之后 fd 和内存都还挂着，直到 DB 对象被 delete 才由
  // unique_ptr 收走。Shutdown.NoFdLeakAcrossReopen 会因为「Close 完
  // 立刻数 fd，比之前多」而红；WAL 没 sync 时 ReopenAccumulatesData
  // 会偶发丢最后几条。
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
