// DBImpl：把 MemTable / WAL / SST / Version / compaction 串起来的那个类。
//
// 这个头文件最重要的内容不是接口，而是**锁归属**：每个成员属于哪一层锁、
// 哪些字段可以无锁读。写之前先读一遍注释，比事后用 TSan 找要快十倍。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lsm/compaction.h"
#include "lsm/db.h"
#include "lsm/dbformat.h"
#include "lsm/env.h"
#include "lsm/log_writer.h"
#include "lsm/memtable.h"
#include "lsm/table_cache.h"
#include "lsm/version_set.h"

namespace lsm {

class WriteBatch;

class DBImpl : public DB {
 public:
  explicit DBImpl(const Options& options);
  ~DBImpl() override;

  // DB::Open 调用。成功返回后后台线程已启动。
  Status Init();

  Status Put(const WriteOptions& o, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions& o, const Slice& key) override;
  Status Get(const ReadOptions& o, const Slice& key, std::string* value) override;
  Status Flush() override;
  Status CompactOnce() override;
  Stats GetStats() override;
  Status Close() override;

 private:
  // ---- 恢复 ----
  Status Recover();
  Status ReplayLog(uint64_t log_number, SequenceNumber* max_seq);

  // ---- 写 ----
  Status Write(const WriteOptions& o, WriteBatch* batch);
  // 锁内：mem_ -> imm_，换新 WAL。成功后必须有人在锁外 FlushImm()
  Status SwitchMemTableLocked();

  // ---- flush / compaction ----
  Status FlushImm();                       // 不持 mu_
  Status DoCompactionWork(Compaction* c);  // 不持 mu_
  void DeleteObsoleteFiles();              // 自己取锁

  // ---- 后台线程 ----
  void MaybeScheduleCompaction();
  void BackgroundLoop();
  void BackgroundCompaction();

  static std::string LogFileName(const std::string& dir, uint64_t number);

  Options options_;
  Env* env_;  // 可能是 FaultInjectionEnv，所以有所有权
  std::unique_ptr<Env> owned_env_;

  // ======================= 以下全部受 mu_ 保护 =======================
  std::mutex mu_;
  std::condition_variable bg_cv_;
  MemTable* mem_ = nullptr;   // 当前可写
  MemTable* imm_ = nullptr;   // 正在 flush（只读）
  uint64_t imm_wal_number_ = 0;
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<log::Writer> log_;
  uint64_t log_number_ = 0;
  // 正在生成、但还没进 MANIFEST 的 SST 编号。
  // 这些文件已经在盘上了，可是任何 Version 都还没引用它们——
  // DeleteObsoleteFiles 必须跳过它们，否则会把刚写好的 SST 当垃圾删掉。
  std::set<uint64_t> pending_outputs_;
  bool bg_scheduled_ = false;
  bool shutting_down_ = false;
  bool closed_ = false;
  Status bg_error_;
  std::condition_variable bg_finished_cv_;  // 后台任务完成的通知（测试用）
  // ==================================================================

  // compaction 互斥：前台 CompactOnce() 和后台线程都可能干活，
  // 两个 compaction 同时跑会把同一批输入文件各写一份输出，
  // L1 出现互相重叠的文件，之后按二分查找就会漏 key。
  // 这把锁**独立于 mu_**：compaction 全程不持 mu_（写盘要几十毫秒）。
  std::mutex compaction_mu_;

  // flush 互斥：前台 Flush() 和后台线程都会尝试把 imm_ 写成 SST。
  // 没有它，两个线程会同时写同一个 imm_（产出两份重复的 SST），
  // 而且先完成的那个把 imm_ 置空后，另一个会在 `imm_->Unref()` 上解引用空指针。
  std::mutex flush_mu_;

  // 以下不需要 mu_（原子或只读）
  std::atomic<SequenceNumber> last_sequence_{0};
  std::thread bg_thread_;
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<TableCache> table_cache_;

  std::atomic<uint64_t> stat_flushes_{0};
  std::atomic<uint64_t> stat_compactions_{0};
  std::atomic<uint64_t> stat_write_stalls_{0};
  std::atomic<uint64_t> stat_gets_{0};
  std::atomic<uint64_t> stat_blocks_read_{0};
  std::atomic<uint64_t> stat_filter_checks_{0};
};

}  // namespace lsm
