// sst_dump：把一个 SST 拆开看。
//
// 这不是调试工具，是**理解工具**：你把文件格式背下来了，但只有当
// 「2748 个块、每块 36 条」这样的真实数字出现在屏幕上，那些字节才变成
// 具体的形状。7.2 节的「自洽性检查」就是靠它做的。
//
// 用法：
//   ./sst_dump /tmp/db3/000004.sst
//   ./sst_dump /tmp/db3/000004.sst --raw          # 打印块内每条 entry
//   ./sst_dump /tmp/db3/000004.sst --block=3      # 只看第 3 个块
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "lsm/db.h"
#include "lsm/dbformat.h"
#include "lsm/env.h"
#include "lsm/filter_block.h"
#include "lsm/format.h"
#include "lsm/iterator.h"
#include "lsm/table.h"

namespace {

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s <file.sst> [--raw] [--block=N]\n"
               "  --raw       print every entry in each data block\n"
               "  --block=N   only inspect block N\n",
               prog);
}

std::string Demangle(const lsm::Slice& ikey) {
  lsm::ParsedInternalKey p;
  if (!lsm::ParseInternalKey(ikey, &p)) return "<corrupt>";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "@%llu:%s", static_cast<unsigned long long>(p.sequence),
                p.type == lsm::kTypeDeletion ? "del" : "put");
  return p.user_key.ToString() + buf;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 1;
  }

  const std::string path = argv[1];
  bool raw = false;
  int only_block = -1;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--raw") == 0) {
      raw = true;
    } else if (std::strncmp(argv[i], "--block=", 8) == 0) {
      only_block = std::atoi(argv[i] + 8);
    } else {
      Usage(argv[0]);
      return 1;
    }
  }

  lsm::Env* env = lsm::Env::Default();
  uint64_t file_size = 0;
  lsm::Status s = env->GetFileSize(path, &file_size);
  if (!s.ok()) {
    std::fprintf(stderr, "GetFileSize(%s): %s\n", path.c_str(), s.ToString().c_str());
    return 1;
  }

  std::unique_ptr<lsm::RandomAccessFile> file;
  s = env->NewRandomAccessFile(path, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
    return 1;
  }

  lsm::Options options;
  std::unique_ptr<lsm::Table> table;
  s = lsm::Table::Open(options, std::move(file), file_size, &table);
  if (!s.ok()) {
    std::fprintf(stderr, "Table::Open: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("file: %s  size: %llu bytes\n", path.c_str(),
              static_cast<unsigned long long>(file_size));
  std::printf("data_blocks: %zu\n", table->NumDataBlocks());

  const lsm::BlockHandle& fh = table->filter_handle();
  if (table->has_filter()) {
    const lsm::FilterBlockReader* f = table->filter();
    std::printf("filter: yes  bytes=%llu  segments=%zu\n",
                static_cast<unsigned long long>(fh.size()),
                f != nullptr ? f->num_filters() : 0);
  } else {
    std::printf("filter: NO (every Get will read a block)\n");
  }

  // 全表扫一遍：条目数、平均 key 长度
  std::unique_ptr<lsm::Iterator> iter(table->NewIterator());
  uint64_t entries = 0;
  uint64_t key_bytes = 0;
  uint64_t value_bytes = 0;

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ++entries;
    key_bytes += iter->key().size();
    value_bytes += iter->value().size();
  }
  if (!iter->status().ok()) {
    std::fprintf(stderr, "iterate: %s\n", iter->status().ToString().c_str());
    return 1;
  }

  std::printf("entries: %llu\n", static_cast<unsigned long long>(entries));
  if (entries > 0) {
    std::printf("avg_internal_key_len: %.1f\n",
                static_cast<double>(key_bytes) / static_cast<double>(entries));
    std::printf("avg_value_len: %.1f\n",
                static_cast<double>(value_bytes) / static_cast<double>(entries));
  }
  if (table->NumDataBlocks() > 0) {
    std::printf("avg_entries_per_block: %.1f\n",
                static_cast<double>(entries) /
                    static_cast<double>(table->NumDataBlocks()));
  }

  if (raw) {
    table->ForEachBlock(only_block < 0 ? 0 : only_block, [&](int index, const lsm::Block& block) {
      if (only_block >= 0 && index != only_block) return;
      std::printf("\n--- block %d  entries=%zu  restarts=%zu ---\n", index,
                  block.CountEntries(), block.NumRestarts());
      lsm::InternalKeyComparator icmp;
      std::unique_ptr<lsm::Iterator> bi(block.NewIterator(&icmp));
      for (bi->SeekToFirst(); bi->Valid(); bi->Next()) {
        std::printf("  %-40s  value_len=%zu\n", Demangle(bi->key()).c_str(),
                    bi->value().size());
      }
    });
  }
  return 0;
}
