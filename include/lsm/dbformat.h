// internal key：user_key + 8 字节尾部（(seq << 8) | type，小端）
//
// 为什么要这个尾部：MemTable / SST 里同一个 user_key 有多个版本，
// 它们必须按「seq 降序」排在一起，这样查找时第一个遇到的就是最新版本。
// 把 seq 放在尾部并用小端编码，能让「seq 大 => 字节串小」自动成立，
// 从而不需要自定义比较器就能让有序结构正确工作（最后 8 字节的字节序比较）。
#pragma once

#include <cstdint>
#include <string>

#include "lsm/slice.h"

namespace lsm {

using SequenceNumber = uint64_t;

enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

// 56 位序号 + 8 位类型
static constexpr SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

// 层数。L0 特殊（文件间 key 可重叠），L1 往下每层内 key 不重叠。
// 7 层 × 每层 10 倍容量，能撑住 TB 级数据；本课程的测试用不到 4 层以上，
// 但把常数定成 7 能让「层数」这个循环边界不被误写死成某个具体值。
static constexpr int kNumLevels = 7;

// 查找时用的"类型"：比所有真实类型都大，保证 Seek 能定位到该 key 的最新版本
static constexpr ValueType kValueTypeForSeek = kTypeValue;

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

// 把 user_key 和 8 字节尾部拼进 dst，返回指向拼接结果的 Slice（指向 dst）
Slice AppendInternalKey(std::string* dst, const ParsedInternalKey& k);

// 解析。internal_key 长度 < 8 时返回 false。
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// 取出 user_key 部分（去掉尾部 8 字节）。调用者必须保证长度 >= 8。
inline Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// 调试用：把 internal key 打印成 "user@seq:type"
std::string InternalKeyDebugString(const Slice& internal_key);

// 比较器：user_key 升序；同 user_key 时 seq 降序（新的在前）
struct InternalKeyComparator {
  // ---------------------------------------------------------------------------
  // TODO(1.1) operator()
  //
  // 输入：两个 internal key
  // 输出：<0 / 0 / >0
  // 约束：先按 user_key 做 memcmp 语义比较；相等时 seq 大的排前面（返回负数）。
  //       写反了会导致 Get 永远读到最旧版本，症状是"更新不生效"。
  // ---------------------------------------------------------------------------
  int operator()(const Slice& a, const Slice& b) const;

  // 写进 MANIFEST，用来识别「这个库是用哪种 key 序建的」。
  // 换了比较器却复用旧库，会让所有索引失效——所以必须记录并校验。
  const char* Name() const { return "lsm.InternalKeyComparator"; }
};

// 拥有内存的 internal key。
//
// FileMetaData / Version 里的 smallest/largest 必须长期有效，而 Slice 只是个
// 视图——把 Slice 存进元数据，指向的内存一释放就成了悬垂指针。所以凡是
// 「要存下来的 key」一律用这个类。
class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey{user_key, s, t});
  }

  // 指向内部字节串，仅在调用者不修改本对象期间有效
  Slice Encode() const { return Slice(rep_); }

  // 长度不足 8 字节时返回 false，且不改动当前内容
  bool DecodeFrom(const Slice& s) {
    if (s.size() < 8) return false;
    rep_.assign(s.data(), s.size());
    return true;
  }

  bool Valid() const { return rep_.size() >= 8; }
  void Clear() { rep_.clear(); }

  Slice user_key() const { return ExtractUserKey(Slice(rep_)); }

  // 取出 seq/type；尾部损坏时返回 false
  bool Parse(SequenceNumber* seq, ValueType* type) const {
    ParsedInternalKey p;
    if (!ParseInternalKey(Slice(rep_), &p)) return false;
    *seq = p.sequence;
    *type = p.type;
    return true;
  }

  std::string DebugString() const { return InternalKeyDebugString(Slice(rep_)); }

 private:
  std::string rep_;
};

// 查找专用的 key：内部持有一个 std::string，Slice 指向自己。
// 这样"临时构造一个查询 key 并传给 Get"是安全的（对象活到 Get 返回）。
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  // 指向完整的 internal key
  Slice memtable_key() const { return Slice(data_, static_cast<size_t>(end_ - data_)); }
  // 指向 user_key 部分
  Slice user_key() const { return Slice(data_, static_cast<size_t>(klen_)); }
  // 指向 internal key 部分（= user_key + 8 字节尾部），即 memtable_key 去掉长度前缀
  Slice internal_key() const { return Slice(data_, static_cast<size_t>(end_ - data_)); }

 private:
  const char* data_;
  const char* end_;
  uint32_t klen_;
  char space_[200];  // 小 key 不用堆分配；超长则用 heap_
  std::string heap_;
};

}  // namespace lsm
