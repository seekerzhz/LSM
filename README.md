# LSM

从零实现一个 LSM-Tree 存储引擎 —— 课程配套的代码骨架。

> 课程正文在 **<https://docs.hanzel.top/lsm>**。这个仓库里只有代码，没有讲解。
> 每一章的思路、方案取舍、检查点命令都在正文里；先读那一章，再回来填这里的 `TODO`。

## 这里有什么

- 一份**能编译、能运行、断言大面积失败**的 C++17 骨架：32 个头文件、27 个 `.cc`、19 个测试文件
- 39 个带编号的 `TODO(0.1)`–`TODO(4.5)`，编号与章节一一对应（完整索引见下方）
- 102 个黑盒测试用例。骨架跑起来是 **5499 项检查、139 处失败**；全填完应该是 **7731 项检查、0 失败**
- 完整的 `CMakeLists.txt`：自动探测 GoogleTest，装不上就用内置的 assert runner

它不是一个「照抄就能过」的作业包。关键算法都没有实现，接口签名和调用关系给全了，实现留给你。

## 快速开始

需要：Linux / macOS / WSL2、GCC 10+ 或 Clang 12+、CMake 3.16+、Git。

```bash
git clone git@github.com:seekerzhz/LSM.git
cd LSM

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

./build/lsm_tests
```

第一次跑，别跑全量——先跑你正在做的那一组：

```term
$ ./build/lsm_tests --filter=Coding
[ RUN  ] Coding.Varint32RoundTrip
    FAIL tests/test_coding.cc:22  v == out
[ FAIL ] Coding.Varint32RoundTrip
...
=== 7 cases, 95 checks, 33 failures ===
```

**这是正常的**：`TODO` 是空的，编码函数返回空字节，解码自然全错。你每填完一个 `TODO`，这个 `failures` 就会掉一截；掉到 `0` 那一组就算过。

分组跑的原因是有些用例会直接 `assert` 掉（比如你还没写 `SeekToFirst()`，第一个 `Next()` 就会触发断言），进程一死，全量的那行汇总就打印不出来。做哪个 Lab 跑哪一组，见正文「实验手册」页每张卡片末尾的验收命令。

全部填完之后，全量跑应该得到：

```text
=== 102 cases, 7731 checks, 0 failures ===
```

## 骨架刚拿到手时，每一组长什么样

下表的左列是刚 clone 下来的实测输出（不是示意），右列是全填完之后：

| 组 | 骨架 | 填完 |
|---|---|---|
| `Coding` | 7 cases, 95 checks, **33 failures** | 7 cases, 100 checks, 0 |
| `Crc32` | 5 cases, 5 checks, **2 failures** | 5 cases, 5 checks, 0 |
| `Env` | 5 cases, 32 checks, 0 | 5 cases, 32 checks, 0 |
| `SkipList` | 3 cases, 15 checks, **12 failures** | 3 cases, 531 checks, 0 |
| `MemTable` | 5 cases, 7 checks, **5 failures** | 5 cases, 20 checks, 0 |
| `Wal` | 5 cases, 184 checks, **5 failures** | 5 cases, 295 checks, 0 |
| `Recovery` | 3 cases, 110 checks, **1 failure** | 3 cases, 309 checks, 0 |
| `Block` | 9 cases, 13 checks, **8 failures** | 9 cases, 32 checks, 0 |
| `Bloom` | 6 cases, 9 checks, **5 failures** | 6 cases, 10 checks, 0 |
| `Table` | 9 cases, 25 checks, **9 failures** | 9 cases, 422 checks, 0 |
| `MergeIterator` | 4 cases, 18 checks, **3 failures** | 4 cases, 18 checks, 0 |
| `DbRead` | 6 cases, 3132 checks, **8 failures** | 6 cases, 3166 checks, 0 |
| `Version` | 8 cases, 94 checks, **8 failures** | 8 cases, 107 checks, 0 |
| `Manifest` | 6 cases, 134 checks, **9 failures** | 6 cases, 189 checks, 0 |
| `Compaction` | 5 cases, 24 checks, **7 failures** | 5 cases, 848 checks, 0 |
| `RefCount` | 6 cases, 37 checks, **13 failures** | 6 cases, 42 checks, 0 |
| `Concurrency` | 5 cases, 1528 checks, **7 failures** | 5 cases, 1530 checks, 0 |
| `Shutdown` | 5 cases, 37 checks, **4 failures** | 5 cases, 75 checks, 0 |
| **全量** | **102 cases, 5499 checks, 139 failures** | **102 cases, 7731 checks, 0 failures** |

「checks」在骨架下会小很多，因为一个用例在第一个失败点就停，后面的断言根本没跑到。所以别拿 `checks` 当进度条——**看 `failures`**。

`Env` 那组骨架下就是 0，不是漏了：它测的是 POSIX 文件读写本身（`Read` / `Append` / `Remove`），这些课程已经给全了。`TODO(0.5)` 的 `fsync` 和 `TODO(0.6)` 的缓冲策略靠后面的 `Recovery` / `Shutdown` 用例间接验证——`fsync` 写错时，`kill -9` 那组会开始偶发丢数据。

没有 GitHub 账号或没配 SSH key？先看正文的 [Git 工作流](https://docs.hanzel.top/lsm/#/git-workflow) 那一章，配一次，后面所有作业都省事。

## 目录

```text
.
├── CMakeLists.txt          # 构建脚本，不需要改
├── include/lsm/            # 32 个头文件：接口签名、不变量、TODO 标记
│   ├── slice.h  status.h  arena.h  coding.h  crc32.h     # 阶段 0 原语
│   ├── env.h  log_format.h  log_writer.h  log_reader.h   # 文件与 WAL
│   ├── dbformat.h  write_batch.h  skiplist.h  memtable.h # 内存侧
│   ├── block.h  block_builder.h  filter_policy.h         # 阶段 2 块格式
│   ├── filter_block.h  table.h  table_builder.h          # 过滤器与 SST
│   ├── merge_iterator.h  iterator.h                      # 迭代器抽象
│   ├── version.h  version_edit.h  version_set.h          # 版本与 MANIFEST
│   ├── compaction.h  compaction_iterator.h               # 阶段 3 压缩
│   ├── table_cache.h  fault_env.h  thread_safe.h         # 配套件
│   └── db.h  db_impl.h                                   # 对外 API
├── src/                    # 27 个 .cc：TODO 所在的地方
├── tests/                  # 19 个测试文件，只看你的公开接口返回什么
│   ├── test_macros.h       # TEST / EXPECT_EQ / ASSERT_TRUE，gtest 兼容
│   ├── test_util.h         # 临时目录、造 SST、造 InternalKey 的小工具
│   └── test_main.cc        # 极简 runner，输出 "=== N cases, M checks, K failures ==="
└── tools/varint_probe.cc   # 手工验证 VarInt 的小工具
```

## TODO 索引

39 个，按阶段排。左边是编号，中间是位置，右边是要实现的东西。编号的第一位就是所属章节。

| 编号 | 位置 | 要实现什么 |
|---|---|---|
| 0.1 | `src/coding.cc` | `PutVarint32` / `PutVarint64` |
| 0.2 | `src/coding.cc` | `GetVarint32` / `GetVarint64` |
| 0.4 | `src/crc32.cc` | `Crc32c` / `Crc32cExtend`，并生成查表 |
| 0.5 | `src/env_posix.cc` | `WritableFile::Sync()`：`fsync` + `EINTR` 重试 |
| 0.6 | `src/env_posix.cc` | `WritableFile::Append()` 的缓冲策略 |
| 1.1 | `src/dbformat.cc` | `InternalKeyComparator::operator()` |
| 1.2 | `include/lsm/skiplist.h` | `RandomHeight()` |
| 1.3 | `include/lsm/skiplist.h` | `FindGreaterOrEqual()` |
| 1.4 | `include/lsm/skiplist.h` | `FindLast()` |
| 1.5 | `include/lsm/skiplist.h` | `Insert()` |
| 1.6 | `src/memtable.cc` | `MemTable::Add()` |
| 1.7 | `src/memtable.cc` | `MemTable::Get()` |
| 1.8 | `src/log_writer.cc` | `Writer::AddRecord()` |
| 1.9 | `src/log_reader.cc` | `Reader::ReadRecord()` |
| 1.12 | `src/write_batch.cc` | `Put` / `Delete` / `Iterate` |
| 2.1 | `src/block_builder.cc` | `BlockBuilder::Add()` |
| 2.2 | `src/block.cc` | `Block::Iter::Seek()` |
| 2.3 | `src/block.cc` | `Block::Iter::Prev()` |
| 2.4 | `src/table_builder.cc` | `TableBuilder::Add()` |
| 2.5 | `src/table_builder.cc` | `TableBuilder::Finish()` |
| 2.6 | `src/table.cc` | `Table::Open()` |
| 2.7 | `src/table.cc` | `Table::InternalGet()` |
| 2.8 | `src/bloom.cc` | `BloomFilterPolicy::CreateFilter()` |
| 2.9 | `src/bloom.cc` | `BloomFilterPolicy::KeyMayMatch()` |
| 2.10 | `include/lsm/merge_iterator.h` | `MergingIterator::FindSmallest()` |
| 2.12 | `src/db_impl.cc` | `SwitchMemTableLocked()` 与 `FlushImm()` |
| 2.13 | `src/filter_block.cc` | `FilterBlockBuilder` 的三个方法 |
| 3.1 | `src/version.cc` | `Version::Get()` |
| 3.2 | `src/version_set.cc` | `VersionSet::Recover()` |
| 3.3 | `src/version_set.cc` | `VersionSet::LogAndApply()` |
| 3.4 | `src/version_set.cc` | `VersionSet::PickCompaction()` |
| 3.5 | `src/version.cc` | `Version::Unref()` 的归零分支 |
| 3.6 | `src/db_impl.cc` | `DBImpl::DoCompactionWork()` |
| 3.7 | `src/compaction_iterator.cc` | `CompactionIterator::FindNextUserEntry()` |
| 4.1 | `src/db_impl.cc` | `DBImpl::Write()`：背压、sync、切表、调度 |
| 4.2 | `src/db_impl.cc` | `DBImpl::Get()`：无锁读的引用计数 |
| 4.3 | `src/db_impl.cc` | `DBImpl::MaybeScheduleCompaction()` |
| 4.4 | `src/db_impl.cc` | `DBImpl::BackgroundLoop()` |
| 4.5 | `src/db_impl.cc` | `DBImpl::Close()` |

编号是稳定的：`TODO(0.2)` 永远指同一件事，你可以放心在提交信息里写 `crc32: 补完 TODO(0.4)`。

### 关于编号里的空档

`0.3`、`1.10`、`1.11`、`2.11` 这四个号缺着，不是漏了：它们在当时的设计里存在过，后来合并或删掉了（`GetLengthPrefixedSlice` 作为工具已经给全，`DBImpl::Recover` 拆成了 `VersionSet::Recover` + `DBImpl::ReplayLog`）。**编号保留原样不重排**，因为正文里的交叉引用和往届同学的笔记都按它写。看到空档就跳过，它们不是隐藏任务。

## 每个阶段的检查点

跑完当前阶段的命令、看到预期结果再往下走。存储引擎的 bug 会累积，第 3 章的症状常常源于第 1 章一个 `fsync` 的位置。

| 阶段 | 命令 | 通过标准 |
|---|---|---|
| 0 · 存储原语 | `./build/lsm_tests --filter=Coding` 等 | `Coding` `Crc32` `Env` 三组 `failures` 为 0；`./build/varint_probe 300` 输出 `[ac 02]` |
| 1 · MemTable 与 WAL | `./build/lsm_tests --filter=Wal` 等 | `SkipList` `MemTable` `Wal` `Recovery` 四组为 0；`kill -9` 后重启不丢已提交数据 |
| 2 · SSTable 与读路径 | `./build/lsm_tests --filter=Table` 等 | `Block` `Bloom` `Table` `MergeIterator` `DbRead` 五组为 0 |
| 3 · Compaction | `./build/lsm_tests --filter=Version` 等 | `Version` `Manifest` `Compaction` `RefCount` 四组为 0 |
| 4 · 并发与关闭 | `./build/lsm_tests --filter=Concurrency` 等 | `Concurrency` `Shutdown` 为 0；再用 TSan 跑一遍零告警 |
| 收尾 · 结课项目 | `./lsm_cli --bench ...` | 四组负载跑完，数字与报告一致 |

阶段 4 的 TSan 跑法：

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan -j
setarch $(uname -m) -R ./build-tsan/lsm_tests --filter=Concurrency
```

`setarch -R` 是关地址随机化。新内核的 ASLR 熵太高时 TSan 会先给你一记 `FATAL: unexpected memory mapping`，那是环境问题，不是你的代码。

## 用 Git 交作业

课程要求用 Git，不是形式主义：这门课的 bug 大多数是**同一个函数改了三版，第三版崩了而你不知道前两版好在哪**。没有提交历史，你只能重写。

约定很简单：

```bash
git switch -c s1-memtable          # 每个阶段开一个分支
# ... 填 TODO，编译通过，测试变绿 ...
git add -p                         # 分块暂存，别把 build/ 和调试代码提交进去
git commit -m "memtable: 补完 TODO(1.6)(1.7)，MemTable 用例 12/12"
git tag s1-done                    # 阶段通过的快照，后面崩了能一键回来
git push -u origin s1-memtable --tags
```

最终提交的是**仓库链接 + tag**，不是压缩包。详细的操作、报错处理、`git bisect` 定位回归，见正文的 Git 工作流一章。

## 一个约定

`TODO` 不要删，填完之后把注释改成一句话说明你的实现选择。评分会看这个——「为什么这里用 `memcpy` 而不是逐字节」比「代码能跑」值钱。

## 许可

课程代码用于教学，可自由取用修改。第三方库一个都没引，全部标准库 + POSIX。
