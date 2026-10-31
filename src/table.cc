#include "lsm/table.h"

#include <cassert>
#include <cstring>

#include "lsm/coding.h"
#include "lsm/table_builder.h"

namespace lsm {

Table::Table(const Options& options, std::unique_ptr<RandomAccessFile> file,
             uint64_t file_size)
    : options_(options),
      file_(std::move(file)),
      file_size_(file_size),
      policy_(NewBloomFilterPolicy(options.bits_per_key)) {}

// ---------------------------------------------------------------------------
// TODO(2.6) Table::Open
//
// 约束：
//   1. file_size < kFooterEncodedLength（48）-> Corruption("file too short")
//   2. 读最后 48 字节 -> Footer::DecodeFrom（它会校验 magic）
//   3. 读 metaindex block，从中找 kFilterMetaKey，把 handle 存进 filter_handle_
//   4. 读 index block：**失败就是 Corruption**，没有索引的 SST 无法使用
//   5. 不读 filter block（懒加载，见 Table::filter）
//
// 为什么 index 必须现在读、filter 可以晚点读：index 是「每次查询都要用」的，
// filter 是「只有可能命中的查询才用」。而且一个刚写完的 SST 可能永远不会
// 被读到，在 Open 时读它的位图是纯浪费。
// ---------------------------------------------------------------------------
Status Table::Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                   uint64_t file_size, std::unique_ptr<Table>* table) {
  (void)options;
  (void)file_size;
  file.reset();
  table->reset();
  // 占位实现：不读 footer、不读 index，直接报错。
  // 按上面 5 步填；注意第 4 步 index 读失败必须原样返回错误，
  // 不能「当成没有索引继续跑」——没有索引的 SST 一次查询就是全表扫。
  return Status::Corruption("Table::Open 还没实现（TODO 2.6）");
}

Status Table::ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* block) const {
  // handle 来自文件内容，可能是损坏的（甚至是恶意的）。先做大小检查，
  // 否则一个 2^63 的 size 会让 resize 直接抛 bad_alloc 或吃掉所有内存。
  if (handle.size() > file_size_ || handle.offset() > file_size_ - handle.size()) {
    return Status::Corruption("block handle out of file bounds");
  }

  std::string buf;
  buf.resize(static_cast<size_t>(handle.size()));
  Slice contents;
  Status s = file_->Read(handle.offset(), static_cast<size_t>(handle.size()), &contents,
                         buf.data());
  if (!s.ok()) return s;
  if (contents.size() != static_cast<size_t>(handle.size())) {
    return Status::Corruption("truncated block read");
  }

  *block = std::make_unique<Block>(contents.ToString());
  if ((*block)->IsCorrupted()) return (*block)->status();
  return Status::OK();
}

void Table::ReadFilter() const {
  if (filter_handle_.size() == kInvalidHandle) return;

  const uint64_t off = filter_handle_.offset();
  const uint64_t sz = filter_handle_.size();
  if (sz > file_size_ || off > file_size_ - sz) return;

  std::string buf;
  buf.resize(static_cast<size_t>(sz));
  Slice contents;
  // 读失败时静默返回：没有 filter 只是变慢，不影响正确性。
  // 把它做成错误会让一个「filter 段恰好损坏」的文件整个不可读。
  Status s = file_->Read(off, static_cast<size_t>(sz), &contents, buf.data());
  if (!s.ok() || contents.size() != static_cast<size_t>(sz)) return;

  filter_reader_ = std::make_unique<FilterBlockReader>(policy_.get(), contents);
}

const FilterBlockReader* Table::filter() const {
  std::lock_guard<std::mutex> lk(filter_mu_);
  if (!filter_loaded_) {
    // 先置标志再读：即使读失败也只尝试一次，不会每次 Get 都重试读盘
    filter_loaded_ = true;
    ReadFilter();
  }
  return filter_reader_.get();
}

// ---------------------------------------------------------------------------
// TODO(2.7) Table::InternalGet
//
// 顺序（每一步都直接影响读放大）：
//   1. index_iter->Seek(internal_key) —— 在内存里，不读盘
//   2. 从命中的索引项里解出 BlockHandle
//   3. filter 检查：用 handle.offset() 定位到对应的 filter 段，
//      用**user_key**（不是 internal key！）去查
//   4. 通过才读 data block
//   5. block_iter->Seek(internal_key)，然后**校验 user_key 相等**
//   6. 按 type 返回：kTypeValue 写 value；kTypeDeletion 清空 value
//      —— 两者都返回 OK，让上层知道「这里有明确答案，不要再往下查」
//
// 三个最容易写错的地方：
//   - 用 internal key 去查 Bloom：尾部的 8 字节 seq 每次都不同，
//     等于每个版本一个不同的 key，假阳性接近 100%
//   - 不校验 user_key 相等：Seek 定位到的是「第一个 >= target」，
//     可以是相邻 key，症状是「查 foo 得到 foobar 的值」
//   - 墓碑返回 NotFound：上层会继续往更旧的文件查，删除就复活了
// ---------------------------------------------------------------------------
Status Table::InternalGet(const ReadOptions& opt, const Slice& key, std::string* value,
                          ValueType* type, GetStats* stats) {
  (void)opt;
  (void)key;
  (void)value;
  (void)type;
  (void)stats;
  // 占位实现：一律说「这块表里没有」。上层会继续往下查，最后返回 NotFound。
  // 按上面 6 步填；三个易错点都在注释里了。
  return Status::NotFound("Table::InternalGet 还没实现（TODO 2.7）");
}

size_t Table::NumDataBlocks() const {
  return index_block_ != nullptr ? index_block_->CountEntries() : 0;
}

void Table::ForEachBlock(int start_index,
                         const std::function<void(int, const Block&)>& fn) const {
  std::unique_ptr<Iterator> iiter(index_block_->NewIterator(&icmp_));
  int index = 0;
  for (iiter->SeekToFirst(); iiter->Valid(); iiter->Next(), ++index) {
    if (index < start_index) continue;
    BlockHandle handle;
    Slice input = iiter->value();
    if (!handle.DecodeFrom(&input).ok()) continue;
    std::unique_ptr<Block> block;
    if (!ReadBlock(handle, &block).ok()) continue;
    fn(index, *block);
  }
}

// ---------------------------------------------------------------------------
// TableIterator：按索引顺序扫全表（compaction 的输入）
// ---------------------------------------------------------------------------
class TableIterator : public Iterator {
 public:
  TableIterator(const Table* table, const InternalKeyComparator* cmp)
      : table_(table),
        cmp_(cmp),
        index_iter_(table->index_block_->NewIterator(&table->icmp_)) {}

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataIter();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataIter();
    if (data_iter_ != nullptr) data_iter_->SeekToLast();
  }

  void Seek(const Slice& target) override {
    index_iter_->Seek(target);
    InitDataIter();
    if (data_iter_ != nullptr) data_iter_->Seek(target);
  }

  void Next() override {
    // 骨架里 data_iter_ 可能还是 nullptr（Open 没实现）：用 if 而不是
    // assert，避免整个测试进程被一个断言带走。
    if (!Valid()) return;
    data_iter_->Next();
    if (!data_iter_->Valid()) {
      index_iter_->Next();
      InitDataIter();
    }
  }

  void Prev() override {
    if (!Valid()) return;
    data_iter_->Prev();
    if (!data_iter_->Valid()) {
      index_iter_->Prev();
      InitDataIter();
      if (data_iter_ != nullptr) data_iter_->SeekToLast();
    }
  }

  Slice key() const override {
    assert(Valid());
    return data_iter_->key();
  }

  Slice value() const override {
    assert(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    if (data_iter_ != nullptr) return data_iter_->status();
    return index_iter_->status();
  }

 private:
  // 把迭代器切到索引当前指向的块
  void InitDataIter() {
    data_iter_.reset();
    data_block_.reset();
    if (!index_iter_->Valid()) return;

    BlockHandle handle;
    Slice input = index_iter_->value();
    Status s = handle.DecodeFrom(&input);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    std::unique_ptr<Block> block;
    s = table_->ReadBlock(handle, &block);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    data_block_ = std::move(block);
    data_iter_.reset(data_block_->NewIterator(cmp_));
    // 新迭代器必须显式定位。少了这句它停在 current_ == restarts_offset_ 的
    // 未初始化状态，Valid() 恒为假：表现是「扫完第一个块就停」，
    // 但块数、条目数统计都正常，非常难查。
    data_iter_->SeekToFirst();
  }

  const Table* table_;
  const InternalKeyComparator* cmp_;
  std::unique_ptr<Iterator> index_iter_;
  // 只保留「当前块」：整表扫描时内存占用是 2 个块，不是 N 个块
  std::unique_ptr<Block> data_block_;
  std::unique_ptr<Iterator> data_iter_;
  Status status_;
};

Iterator* Table::NewIterator() const {
  return new TableIterator(this, &icmp_);
}

}  // namespace lsm
