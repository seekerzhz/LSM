#include "lsm/table_cache.h"

#include <cstdio>

namespace lsm {

std::string TableCache::FileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.sst", static_cast<unsigned long long>(number));
  return dbname + buf;
}

TableCache::TableCache(const Options& options, const std::string& dbname)
    : options_(options), dbname_(dbname) {}

TableCache::~TableCache() { Clear(); }

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Table** table) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(file_number);
    if (it != cache_.end()) {
      *table = it->second.get();
      return Status::OK();
    }
  }

  // 打开是不在锁内做的：open + read 可能要几毫秒，持锁会卡住所有读者。
  // 代价是并发首次访问同一文件时可能重复打开一次（无害，后到的覆盖先到的）。
  Env* env = Env::Default();
  std::unique_ptr<RandomAccessFile> file;
  Status s = env->NewRandomAccessFile(FileName(dbname_, file_number), &file);
  if (!s.ok()) return s;

  std::unique_ptr<Table> t;
  s = Table::Open(options_, std::move(file), file_size, &t);
  if (!s.ok()) return s;

  std::shared_ptr<Table> shared(std::move(t));
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(file_number);
  if (it != cache_.end()) {
    // 别人抢先打开了：用已有的那份，让 shared 自然析构
    *table = it->second.get();
    return Status::OK();
  }
  cache_[file_number] = shared;
  *table = shared.get();
  return Status::OK();
}

Status TableCache::Get(const ReadOptions& opt, uint64_t file_number, uint64_t file_size,
                       const Slice& internal_key, std::string* value, ValueType* type,
                       GetStats* stats) {
  Table* t = nullptr;
  Status s = FindTable(file_number, file_size, &t);
  if (!s.ok()) return s;
  return t->InternalGet(opt, internal_key, value, type, stats);
}

namespace {

// 让迭代器持有 Table 的 shared_ptr，这样即使中途 Evict 也不会 use-after-free。
// compaction 可能要扫一个文件好几分钟，期间版本切换删掉这个文件是正常的。
class TableOwningIterator : public Iterator {
 public:
  TableOwningIterator(std::shared_ptr<Table> table, Iterator* inner)
      : table_(std::move(table)), inner_(inner) {}

  ~TableOwningIterator() override { delete inner_; }

  bool Valid() const override { return inner_->Valid(); }
  void SeekToFirst() override { inner_->SeekToFirst(); }
  void SeekToLast() override { inner_->SeekToLast(); }
  void Seek(const Slice& target) override { inner_->Seek(target); }
  void Next() override { inner_->Next(); }
  void Prev() override { inner_->Prev(); }
  Slice key() const override { return inner_->key(); }
  Slice value() const override { return inner_->value(); }
  Status status() const override { return inner_->status(); }

 private:
  std::shared_ptr<Table> table_;
  Iterator* inner_;
};

}  // namespace

Iterator* TableCache::NewIterator(uint64_t file_number, uint64_t file_size) {
  Table* t = nullptr;
  Status s = FindTable(file_number, file_size, &t);
  if (!s.ok()) {
    // 返回一个「立刻无效且带错误」的迭代器，而不是 nullptr：
    // 调用方（compaction）总会 delete 迭代器，返回 nullptr 会让它在别处崩。
    return NewErrorIterator(s);
  }

  std::shared_ptr<Table> shared;
  {
    std::lock_guard<std::mutex> lk(mu_);
    shared = cache_[file_number];
  }
  return new TableOwningIterator(shared, t->NewIterator());
}

void TableCache::Evict(uint64_t file_number) {
  std::shared_ptr<Table> victim;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(file_number);
    if (it == cache_.end()) return;
    victim = it->second;
    cache_.erase(it);
  }
  // victim 在这里析构（除非还有迭代器持有）。fd 随 Table 的析构关闭。
}

void TableCache::Clear() {
  std::unordered_map<uint64_t, std::shared_ptr<Table>> tmp;
  {
    std::lock_guard<std::mutex> lk(mu_);
    tmp.swap(cache_);
  }
  // tmp 在锁外析构：析构会 close 大量 fd，持锁做这个没必要
}

size_t TableCache::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cache_.size();
}

}  // namespace lsm
