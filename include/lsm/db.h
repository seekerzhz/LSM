// DB 门面。
//
// 五个阶段共用这一个接口：阶段 1 只有 MemTable + WAL，阶段 2 加上 SST，
// 阶段 3 加上 compaction，阶段 4 加上后台线程。每一步都不改调用方代码。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/dbformat.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

struct Options {
  // 数据库目录。必须已存在或允许创建。
  std::string db_path;

  // MemTable 达到这个字节数就 flush 成 SST（阶段 2 起生效）
  size_t write_buffer_size = 4u << 20;

  // 单个 SST 的目标大小。超过就切下一个文件（compaction 输出用）
  size_t max_file_size = 2u << 20;

  // data block 的目标大小。块越大，索引越小、压缩率越高，但读放大越粗
  size_t block_size = 4096;

  // Bloom filter 的每 key 比特数。10 约等于 1% 假阳性
  int bits_per_key = 10;

  // L0 文件数达到这个值就触发 compaction
  int l0_compaction_trigger = 4;
  // L0 文件数达到这个值就阻塞写入（write stall），等 compaction 追上
  int l0_stop_writes_trigger = 12;

  bool create_if_missing = true;
  bool error_if_exists = false;

  // 测试用：为 true 时 Env::Default() 换成故障注入版本（见 fault_env.h）
  bool use_fault_env = false;
};

struct WriteOptions {
  // true：写成功后调用 fsync，断电也不丢。
  // false：只保证数据进了内核页缓存（kill -9 不丢，断电会丢）。
  bool sync = false;
};

struct ReadOptions {
  // 0 表示读最新。>0 表示读该 seq 的快照（MVCC，阶段 3 起可用）。
  SequenceNumber snapshot = 0;
};

// 运行期统计。压测工具与测试用它做断言（阶段 4 起才有真实数字）。
struct Stats {
  uint64_t flushes = 0;       // MemTable -> SST 的次数
  uint64_t compactions = 0;   // 完成的 compaction 轮数
  uint64_t write_stalls = 0;  // 因 L0 文件过多而阻塞写入的次数
  int num_sst_files = 0;      // 当前版本里的 SST 总数
  uint64_t disk_usage = 0;    // 这些 SST 的字节数之和

  // 每层的文件数。compaction 有没有真的把 L0 压成 L1，只能靠它看出来——
  // 「文件总数没变」既可能是没压，也可能是压成了一样多的文件。
  int level_files[kNumLevels] = {0};

  // 读放大诊断：avg_blocks_read = blocks_read / gets
  // 健康值接近 1.0；如果是 5.0，说明 Bloom filter 没生效。
  uint64_t gets = 0;
  uint64_t blocks_read = 0;
  uint64_t filter_checks = 0;
};

class DB {
 public:
  static Status Open(const Options& options, DB** dbptr);

  virtual Status Put(const WriteOptions& o, const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& o, const Slice& key) = 0;
  virtual Status Get(const ReadOptions& o, const Slice& key, std::string* value) = 0;

  // 把当前 MemTable 落成一个 SST（阶段 2 起有实现）。
  virtual Status Flush() = 0;

  // 强制跑一轮 compaction（测试与压测用）。
  virtual Status CompactOnce() = 0;

  virtual Stats GetStats() = 0;

  // 显式关闭。析构会自动调用，但测试需要「关掉再打开」来验证恢复。
  // 返回后保证后台线程已 join、所有文件已 sync、句柄已释放。
  virtual Status Close() = 0;

  virtual ~DB() = default;
};

}  // namespace lsm
