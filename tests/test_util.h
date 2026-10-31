// 测试辅助（上层）：直接造 SST 文件，绕过 DB。
//
// 定宽键、值、internal key 那些最底层的构造在 key_util.h 里，
// 这一份只放「需要 SSTable 才能存在」的东西。
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsm/dbformat.h"
#include "lsm/env.h"
#include "lsm/table_builder.h"
#include "lsm/table_cache.h"
#include "key_util.h"

namespace lsm_test {

// ---------------------------------------------------------------------------
// 直接造一个 SST 文件（绕过 DB，阶段 3 的版本/compaction 测试要用）
//
// kvs 里的 user_key 必须已经升序；所有条目共用同一个 seq —— 这样
// 「同一个 key 的多个版本」不会混进这一层，测试的关注点才单一。
// ---------------------------------------------------------------------------
struct SstInfo {
  uint64_t number = 0;
  uint64_t file_size = 0;
  lsm::InternalKey smallest;
  lsm::InternalKey largest;
  bool ok = false;
};

inline SstInfo WriteSst(const lsm::Options& options, const std::string& dbname,
                        uint64_t number,
                        const std::vector<std::pair<std::string, std::string>>& kvs,
                        lsm::SequenceNumber seq,
                        lsm::ValueType type = lsm::kTypeValue) {
  SstInfo info;
  info.number = number;

  std::unique_ptr<lsm::WritableFile> file;
  const std::string path = lsm::TableCache::FileName(dbname, number);
  if (!lsm::Env::Default()->NewWritableFile(path, &file).ok()) return info;

  lsm::TableBuilder builder(options, file.get());
  bool first = true;
  for (const auto& kv : kvs) {
    const std::string ikey = IKey(kv.first, seq, type);
    if (first) {
      info.smallest.DecodeFrom(ikey);
      first = false;
    }
    info.largest.DecodeFrom(ikey);
    builder.Add(lsm::Slice(ikey), lsm::Slice(kv.second));
  }

  if (!builder.Finish().ok()) return info;
  if (!file->Sync().ok()) return info;
  info.file_size = builder.FileSize();
  info.ok = true;
  return info;
}

// 生成 [lo, hi) 的连续键值对
inline std::vector<std::pair<std::string, std::string>> Range(uint64_t lo, uint64_t hi) {
  std::vector<std::pair<std::string, std::string>> out;
  for (uint64_t i = lo; i < hi; ++i) out.emplace_back(Key(i), Value(i));
  return out;
}

}  // namespace lsm_test
