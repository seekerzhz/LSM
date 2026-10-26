#include "lsm/iterator.h"

namespace lsm {
namespace {

class ErrorIterator : public Iterator {
 public:
  explicit ErrorIterator(const Status& s) : status_(s) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}
  void Next() override {}
  void Prev() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Iterator* NewErrorIterator(const Status& s) { return new ErrorIterator(s); }

}  // namespace lsm
