#include "lsm/block_builder.h"

#include <algorithm>
#include <cassert>

#include "lsm/coding.h"

namespace lsm {

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  // 第一个重启点永远在偏移 0。没有它，块内第一条 entry 的 shared 就没有
  // 「参照物」——它的 shared 必须是 0，而 restarts_[0] = 0 正是这个约定。
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
  num_entries_ = 0;
}

// ---------------------------------------------------------------------------
// TODO(2.1) BlockBuilder::Add
//
// 输入：key（必须 >= 上一条 key）、value
// 输出：把一条 entry 追加进 buffer_
// 约束：
//   1. 算 shared = 与 last_key_ 的公共前缀长度
//   2. 若 counter_ 已达 kRestartInterval：先把当前 buffer_ 偏移记为重启点，
//      counter_ 归零，本次的 shared 必须为 0（否则无法从重启点独立解析）
//   3. 写三个 VarInt32（shared / non_shared / value_len）+ key 后缀 + value
//   4. 更新 last_key_、counter_、num_entries_
// 注意：顺序不能反——先记重启点再算 shared，否则重启点位置会错一条。
// ---------------------------------------------------------------------------
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  assert(counter_ <= kRestartInterval);
  const Slice last_key_piece(last_key_);
  // 块内必须严格有序。这里要用**比较器**而不是 memcmp：internal key 的
  // 尾部 8 字节是小端 seq，字节序与「seq 大小」不单调。用 memcmp 会在
  // 「同一个 user_key 的两个版本」上误报，让正确的写入被断言拦下来。
  assert(last_key_.empty() || Compare(key, last_key_piece) >= 0);

  size_t shared = 0;
  if (counter_ < kRestartInterval) {
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while (shared < min_length && last_key_piece[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // 该开新组了。记下当前位置，本条 entry 的 shared 保持 0。
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const size_t non_shared = key.size() - shared;
  char buf[40];  // 三个 VarInt32 最多 15 字节，留足余量
  buffer_.append(buf, PutVarint32(buf, static_cast<uint32_t>(shared)));
  buffer_.append(buf, PutVarint32(buf, static_cast<uint32_t>(non_shared)));
  buffer_.append(buf, PutVarint32(buf, static_cast<uint32_t>(value.size())));
  buffer_.append(key.data() + shared, non_shared);
  if (!value.empty()) buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++counter_;
  ++num_entries_;
}

Slice BlockBuilder::Finish() {
  // 重启点数组放在 entries 之后，每个 4 字节固定宽度。
  // 定长是关键：读者从块尾往前数 4*count 字节就能找到整个数组，
  // 不需要知道每个重启点的编码长度。
  for (uint32_t r : restarts_) {
    char buf[4];
    EncodeFixed32(buf, r);
    buffer_.append(buf, 4);
  }
  char buf[4];
  EncodeFixed32(buf, static_cast<uint32_t>(restarts_.size()));
  buffer_.append(buf, 4);

  finished_ = true;
  return Slice(buffer_);
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  // entries + 重启点数组 + 4 字节的个数。
  // 再预留一条 entry 的量（这里取 40 字节的粗略上界），
  // 否则「刚好差一点点就超」的情况会让最后一块略超标。
  return buffer_.size() + restarts_.size() * 4 + 4 + (counter_ > 0 ? 40 : 0);
}

}  // namespace lsm
