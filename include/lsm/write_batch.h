// WriteBatch：一批写入的序列化形式。
//
// 它同时是 WAL 的 payload 格式和「原子批量写」的单位。第 3 章的 group commit
// 和一个 Put 内部其实就是「只有一条记录的 WriteBatch」。
//
// 字节布局：
//   [seq:uint64 小端][count:uint32 小端]
//   [count 组: type(1) + len_prefix(key) + (type==kTypeValue ? len_prefix(value) : 空)]
#pragma once

#include <cstdint>
#include <string>

#include "lsm/dbformat.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

class WriteBatch {
 public:
  WriteBatch() { Clear(); }

  // 清空成"空前缀"：seq=0, count=0
  void Clear();

  // 用一个已有的字节串（从 WAL 读出来的那一段）整体替换内容。
  // 恢复时用：把 ReadRecord 拿到的 payload 直接塞进来。
  // 只校验最小长度，不解析条目（条目在 Iterate 时才校验）。
  Status SetContents(const Slice& data);

  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);

  // 逐条遍历。Handler 返回 ok 才继续。
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual Status Put(const Slice& key, const Slice& value) = 0;
    virtual Status Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

  // 整个 batch 的字节串（WAL payload）
  Slice Repr() const { return Slice(rep_); }
  size_t ByteSize() const { return rep_.size(); }

  SequenceNumber Sequence() const;
  void SetSequence(SequenceNumber seq);

  // 批内操作条数（存在头部）
  int Count() const;

  // 估算字节数（第 4 章内存控制用）
  size_t ApproximateSize() const { return rep_.size(); }

 private:
  std::string rep_;
};

}  // namespace lsm
