// Slice: a non-owning view of a byte range.
//
// 本课程里所有「字符串参数」都用 Slice 而不是 const std::string&，
// 原因：调用方经常只有 char* + len，或者只想传 std::string 的一部分。
// Slice 不拥有内存，因此指向的内存在使用期间必须有效。
#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace lsm {

class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}  // NOLINT
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}          // NOLINT

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](size_t n) const { return data_[n]; }

  void clear() {
    data_ = "";
    size_ = 0;
  }

  void remove_prefix(size_t n) {
    data_ += n;
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }

  // 字典序比较，语义与 memcmp 一致（返回 <0 / 0 / >0）
  int compare(const Slice& b) const;

  bool starts_with(const Slice& x) const {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

// 课程提供的实现（完整）
inline int Slice::compare(const Slice& b) const {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = std::memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size_) r = -1;
    else if (size_ > b.size_) r = 1;
  }
  return r;
}

}  // namespace lsm
