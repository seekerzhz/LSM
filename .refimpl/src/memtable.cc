// 参考实现（教师版），仅用于验证骨架。
#include "lsm/memtable.h"

#include <cstring>
#include <new>

#include "lsm/coding.h"

namespace lsm {

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator(EntryKey(a), EntryKey(b));
}

namespace {

const char* EncodeTempKey(std::string* scratch, const Slice& target) {
  MemEntry e{};
  e.key_len = static_cast<uint32_t>(target.size());
  e.tag = 0;
  e.value_len = 0;
  scratch->assign(reinterpret_cast<const char*>(&e), sizeof(MemEntry));
  scratch->append(target.data(), target.size());
  return scratch->data();
}

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(SkipList<const char*, MemTable::KeyComparator>::Iterator iter)
      : iter_(iter) {}
  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Seek(const Slice& target) override {
    tmp_.clear();
    iter_.Seek(EncodeTempKey(&tmp_, target));
  }
  void Next() override { iter_.Next(); }
  void Prev() override { (void)0; }
  Slice key() const override { return MemTable::EntryKey(iter_.key()); }
  Slice value() const override { return MemTable::EntryValue(iter_.key()); }
  Status status() const override { return Status::OK(); }

 private:
  SkipList<const char*, MemTable::KeyComparator>::Iterator iter_;
  std::string tmp_;
};

}  // namespace

MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp), arena_(), table_(comparator_, &arena_), refs_(0) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  std::string ikey;
  ParsedInternalKey p;
  p.user_key = key;
  p.sequence = seq;
  p.type = type;
  AppendInternalKey(&ikey, p);

  const size_t ksize = ikey.size();
  const size_t total = sizeof(MemEntry) + ksize + value.size();
  char* mem = arena_.AllocateAligned(total);
  MemEntry* e = new (mem) MemEntry();
  e->key_len = static_cast<uint32_t>(ksize);
  e->tag = (seq << 8) | static_cast<uint64_t>(type);
  e->value_len = static_cast<uint32_t>(value.size());
  std::memcpy(mem + sizeof(MemEntry), ikey.data(), ksize);
  if (!value.empty()) {
    std::memcpy(mem + sizeof(MemEntry) + ksize, value.data(), value.size());
  }
  table_.Insert(mem);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Iterator* it = NewIterator();
  it->Seek(key.internal_key());
  bool found = false;
  if (it->Valid()) {
    ParsedInternalKey p;
    if (!ParseInternalKey(it->key(), &p)) {
      *s = Status::Corruption("bad internal key in memtable");
      found = true;
    } else if (p.user_key == key.user_key()) {
      if (p.type == kTypeDeletion) {
        *s = Status::NotFound("deleted");
      } else {
        *value = it->value().ToString();
        *s = Status::OK();
      }
      found = true;
    }
  }
  delete it;
  return found;
}

Iterator* MemTable::NewIterator() {
  return new MemTableIterator(SkipList<const char*, KeyComparator>::Iterator(&table_));
}

}  // namespace lsm
