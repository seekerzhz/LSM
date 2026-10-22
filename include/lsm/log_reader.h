// WAL 读取端。
//
// 负责三件容易混淆的事：
//   1. 正常读出一条完整记录
//   2. 识别"文件末尾半条记录"（崩溃残留，应静默丢弃，继续启动）
//   3. 识别"文件中间损坏"（CRC 错、长度非法，应报 Corruption，拒绝启动）
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/env.h"
#include "lsm/log_format.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {
namespace log {

// 出错时把上下文带回去，便于上层打印"哪个文件、第几个字节"
struct Reporter {
  Status* status = nullptr;
  std::string* fname = nullptr;
  uint64_t* last_record_offset = nullptr;
};

class Reader {
 public:
  Reader(SequentialFile* file, Reporter* reporter, bool checksum = true,
         uint64_t initial_offset = 0);

  // 读下一条完整记录。
  //  true  -> *record 指向内部缓冲（下次调用前有效）
  //  false -> 到达文件尾（*status 保持 ok）或发生损坏（*status 被设置）
  bool ReadRecord(Slice* record, std::string* scratch);

  uint64_t LastRecordOffset() const { return last_record_offset_; }

 private:
  // 课程提供（完整）：把下一个块读进 backing_store_，buffer_ 指向它。
  // 返回 false 表示 EOF。
  bool ReadBlock();

  SequentialFile* file_;
  Reporter* reporter_;
  bool checksum_;
  uint64_t initial_offset_;
  uint64_t last_record_offset_;     // 上一条记录在文件中的起始偏移
  uint64_t block_start_offset_;     // 当前块在文件中的起始偏移
  std::string backing_store_;       // 当前块的内容
  Slice buffer_;
  bool eof_;
};

}  // namespace log
}  // namespace lsm
