#include "lsm/compaction_iterator.h"

#include <cassert>

namespace lsm {

CompactionIterator::CompactionIterator(Iterator* input, const InternalKeyComparator* icmp,
                                       Compaction* compaction)
    : input_(input), icmp_(icmp), compaction_(compaction) {}

// ---------------------------------------------------------------------------
// TODO(3.7) CompactionIterator::FindNextUserEntry
//
// 从 input_ 当前指向的记录开始，决定「这一条要不要输出」：
//
//   1. 解析失败 -> status_ 置 Corruption，结束
//   2. 如果 user_key 与上一条**输出过**的相同 -> 丢弃（被更新的版本覆盖）
//   3. 否则这是一个新的 user_key，记为 current_user_key_
//   4. 如果它是墓碑且 compaction_->IsBaseLevelForKey(user_key)：
//        丢弃它，并且后续同 key 的记录也都会因第 2 条被丢弃
//        （下面没有更旧版本了，墓碑留着只会占空间）
//   5. 否则输出这条
//
// 注意 IsBaseLevelForKey 要求按 user_key 升序调用 —— 归并流正好保证这点，
// 所以不能让 Seek(target) 之后乱序调用（本课程只做正向流）。
//
// 想一想：如果某个 key 的墓碑**不在**这次 compaction 的输入范围内
// （它在更高的层），而旧版本在输入里，你的逻辑会怎么处理？
// （答案：正常保留旧版本。因为更高的层优先，读的时候会先命中墓碑。
//   这是安全的，代价是空间没被回收 —— 等那层也被 compact 时才回收。）
// ---------------------------------------------------------------------------
void CompactionIterator::FindNextUserEntry() {
  // 占位实现：一条都不丢，把输入流原样透传。
  //
  // 后果：compaction 的输出和输入一样大 —— 覆盖写和墓碑一个都清不掉。
  //   Compaction.DropsOverwrittenVersions / TombstoneDroppedAtBaseLevel 会红；
  //   Compaction.KeepsEveryLiveKey 反而会过（它检查的是「别丢」）。
  //
  // 按上面 5 条写。第 2 条和第 4 条都是「跳过」，区别是：
  // 前者跳过的是被覆盖的旧版本，后者跳过的是「下面已经没有任何版本」的墓碑。
  valid_ = input_->Valid();
}

void CompactionIterator::SeekToFirst() {
  input_->SeekToFirst();
  have_current_user_key_ = false;
  current_user_key_.clear();
  FindNextUserEntry();
}

void CompactionIterator::Seek(const Slice& target) {
  input_->Seek(target);
  have_current_user_key_ = false;
  current_user_key_.clear();
  FindNextUserEntry();
}

void CompactionIterator::Next() {
  if (!valid_) return;
  input_->Next();
  FindNextUserEntry();
}

Slice CompactionIterator::key() const {
  assert(valid_);
  return input_->key();
}

Slice CompactionIterator::value() const {
  assert(valid_);
  return input_->value();
}

Status CompactionIterator::status() const {
  if (!status_.ok()) return status_;
  return input_->status();
}

void CompactionIterator::SeekToLast() {
  // compaction 只需要正向流。反向会让 IsBaseLevelForKey 的游标失效，
  // 明确不支持比「悄悄给出错误结果」更好。
  assert(false && "CompactionIterator does not support reverse iteration");
  valid_ = false;
}

void CompactionIterator::Prev() {
  assert(false && "CompactionIterator does not support reverse iteration");
  valid_ = false;
}

}  // namespace lsm
