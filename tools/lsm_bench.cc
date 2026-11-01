// lsm_bench：压测与诊断。
//
// 七个模式各有各的用途，不是「一个 benchmark 跑所有」：
//   write  / flush   产生数据，配合 sst_dump 看文件格式
//   read             测读放大（avg_blocks_read 应该接近 1.0）
//   mixed            多线程读写 + 观测 p99，找并发 bug
//   quick            写完立刻读回来校验，冒烟测试
//   fdleak           反复开关库，验句柄泄漏
//   close-mid        在 compaction 跑到一半时关库，验优雅退出
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "latency.h"
#include "lsm/db.h"

namespace {

struct Config {
  std::string db = "/tmp/lsm_bench";
  std::string mode = "write";
  uint64_t n = 100000;
  size_t value_size = 100;
  int threads = 4;
  int duration = 10;
  uint64_t keys_from = 0;
  bool close_mid = false;
  int iterations = 20;
  size_t write_buffer = 4u << 20;
};

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [options]\n"
               "  --db=PATH             database directory (default /tmp/lsm_bench)\n"
               "  --mode=MODE           write|flush|read|mixed|quick|fdleak|close-mid\n"
               "  --n=N                 number of keys\n"
               "  --value-size=BYTES    value length\n"
               "  --threads=N           threads for mixed mode\n"
               "  --duration=SECONDS    mixed mode duration\n"
               "  --keys-from=N         read mode: starting key index\n"
               "  --iterations=N        fdleak mode: open/close cycles\n"
               "  --write-buffer=BYTES  memtable size (small value forces more flushes)\n",
               prog);
}

bool ParseArgs(int argc, char** argv, Config* c) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto val = [&](const char* key) -> const char* {
      const size_t len = std::strlen(key);
      if (std::strncmp(a, key, len) == 0) return a + len;
      return nullptr;
    };
    if (const char* v = val("--db=")) c->db = v;
    else if (const char* v2 = val("--mode=")) c->mode = v2;
    else if (const char* v3 = val("--n=")) c->n = std::strtoull(v3, nullptr, 10);
    else if (const char* v4 = val("--value-size=")) c->value_size = static_cast<size_t>(std::atoi(v4));
    else if (const char* v5 = val("--threads=")) c->threads = std::atoi(v5);
    else if (const char* v6 = val("--duration=")) c->duration = std::atoi(v6);
    else if (const char* v7 = val("--keys-from=")) c->keys_from = std::strtoull(v7, nullptr, 10);
    else if (const char* v8 = val("--iterations=")) c->iterations = std::atoi(v8);
    else if (const char* v9 = val("--write-buffer=")) c->write_buffer = static_cast<size_t>(std::strtoull(v9, nullptr, 10));
    else if (std::strcmp(a, "--close-mid-compaction") == 0) c->close_mid = true;
    else if (std::strcmp(a, "--help") == 0) return false;
    else {
      std::fprintf(stderr, "unknown option: %s\n", a);
      return false;
    }
  }
  return true;
}

std::string MakeKey(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010llu", static_cast<unsigned long long>(i));
  return std::string(buf);
}

std::string MakeValue(uint64_t i, size_t len) {
  // 让 value 内容随 key 变化：如果实现有「读错了 key 但值恰好相等」
  // 这类 bug，固定内容会把它藏起来。
  std::string v(len, 'x');
  char head[32];
  std::snprintf(head, sizeof(head), "%llu:", static_cast<unsigned long long>(i));
  const size_t hn = std::strlen(head);
  for (size_t k = 0; k < hn && k < len; ++k) v[k] = head[k];
  return v;
}

int CountFds() {
  DIR* d = opendir("/proc/self/fd");
  if (d == nullptr) return -1;
  int n = 0;
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] != '.') ++n;
  }
  closedir(d);
  return n;
}

double ElapsedSeconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

lsm::Options MakeOptions(const Config& c) {
  lsm::Options o;
  o.db_path = c.db;
  o.write_buffer_size = c.write_buffer;
  return o;
}

int RunWrite(const Config& c) {
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(MakeOptions(c), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open: %s\n", s.ToString().c_str());
    return 1;
  }
  // 预热：先写一遍，让后续的 write 成为「覆盖写」，
  // 这样 compaction 才有旧版本可丢（空间放大的测试依赖它）
  const auto start = std::chrono::steady_clock::now();
  lsm::WriteOptions wo;
  for (uint64_t i = 0; i < c.n; ++i) {
    const std::string k = MakeKey(i);
    const std::string v = MakeValue(i, c.value_size);
    s = db->Put(wo, k, v);
    if (!s.ok()) {
      std::fprintf(stderr, "Put(%s): %s\n", k.c_str(), s.ToString().c_str());
      db->Close();
      delete db;
      return 1;
    }
  }
  const double sec = ElapsedSeconds(start);
  lsm::Stats st = db->GetStats();
  std::printf("wrote %llu keys in %.2fs (%.0f/s)  value_size=%zu\n",
              static_cast<unsigned long long>(c.n), sec, c.n / (sec > 0 ? sec : 1),
              c.value_size);
  std::printf("flushes=%llu  compactions=%llu  sst_files=%d  disk=%.1fMB\n",
              static_cast<unsigned long long>(st.flushes),
              static_cast<unsigned long long>(st.compactions), st.num_sst_files,
              static_cast<double>(st.disk_usage) / (1024.0 * 1024.0));
  db->Close();
  delete db;
  return 0;
}

int RunFlush(const Config& c) {
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(MakeOptions(c), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open: %s\n", s.ToString().c_str());
    return 1;
  }
  s = db->Flush();
  if (!s.ok()) std::fprintf(stderr, "Flush: %s\n", s.ToString().c_str());
  lsm::Stats st = db->GetStats();
  std::printf("flush done  sst_files=%d  disk=%.2fMB\n", st.num_sst_files,
              static_cast<double>(st.disk_usage) / (1024.0 * 1024.0));
  db->Close();
  delete db;
  return s.ok() ? 0 : 1;
}

int RunRead(const Config& c) {
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(MakeOptions(c), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open: %s\n", s.ToString().c_str());
    return 1;
  }

  lsm::LatencyHistogram hist;
  lsm::ReadOptions ro;
  uint64_t hits = 0;
  uint64_t misses = 0;

  for (uint64_t i = 0; i < c.n; ++i) {
    const std::string k = MakeKey(c.keys_from + i);
    std::string v;
    const auto t0 = std::chrono::steady_clock::now();
    s = db->Get(ro, k, &v);
    const auto t1 = std::chrono::steady_clock::now();
    hist.Record(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    if (s.ok()) {
      ++hits;
    } else if (s.IsNotFound()) {
      ++misses;
    } else {
      std::fprintf(stderr, "Get(%s): %s\n", k.c_str(), s.ToString().c_str());
      break;
    }
  }

  lsm::Stats st = db->GetStats();
  const double avg_blocks = st.gets > 0 ? static_cast<double>(st.blocks_read) / st.gets : 0;
  const double avg_filters = st.gets > 0 ? static_cast<double>(st.filter_checks) / st.gets : 0;

  std::printf("hits=%llu  misses=%llu\n", static_cast<unsigned long long>(hits),
              static_cast<unsigned long long>(misses));
  std::printf("avg_blocks_read=%.2f  avg_filter_checks=%.2f\n", avg_blocks, avg_filters);
  std::printf("p50=%lluus  p90=%lluus  p99=%lluus  p999=%lluus  max=%lluus\n",
              static_cast<unsigned long long>(hist.Percentile(50)),
              static_cast<unsigned long long>(hist.Percentile(90)),
              static_cast<unsigned long long>(hist.Percentile(99)),
              static_cast<unsigned long long>(hist.Percentile(99.9)),
              static_cast<unsigned long long>(hist.max()));

  if (avg_blocks > 3.0) {
    std::printf("NOTE: avg_blocks_read %.2f 偏高 —— 检查 Bloom filter 是否真的写进去了\n",
                avg_blocks);
  }
  db->Close();
  delete db;
  return 0;
}

int RunQuick(const Config& c) {
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(MakeOptions(c), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open: %s\n", s.ToString().c_str());
    return 1;
  }
  for (uint64_t i = 0; i < c.n; ++i) {
    s = db->Put(lsm::WriteOptions(), MakeKey(i), MakeValue(i, c.value_size));
    if (!s.ok()) break;
  }
  if (s.ok()) s = db->Flush();

  uint64_t checked = 0;
  uint64_t bad = 0;
  for (uint64_t i = 0; i < c.n && s.ok(); ++i) {
    std::string v;
    s = db->Get(lsm::ReadOptions(), MakeKey(i), &v);
    if (s.IsNotFound()) {
      ++bad;
      continue;
    }
    if (!s.ok()) break;
    if (v != MakeValue(i, c.value_size)) ++bad;
    ++checked;
  }
  std::printf("quick: n=%llu checked=%llu mismatched=%llu\n",
              static_cast<unsigned long long>(c.n),
              static_cast<unsigned long long>(checked),
              static_cast<unsigned long long>(bad));
  db->Close();
  delete db;
  return (s.ok() && bad == 0) ? 0 : 1;
}

int RunMixed(const Config& c) {
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(MakeOptions(c), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "Open: %s\n", s.ToString().c_str());
    return 1;
  }

  const int writers = c.threads / 2 > 0 ? c.threads / 2 : 1;
  const int readers = c.threads - writers > 0 ? c.threads - writers : 1;

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> puts{0};
  std::atomic<uint64_t> gets{0};
  std::atomic<uint64_t> errors{0};
  lsm::LatencyHistogram put_hist;
  lsm::LatencyHistogram get_hist;

  const int rss_start_kb = [] {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return 0;
    char line[256];
    int kb = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::sscanf(line, "VmRSS: %d kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
  }();

  std::vector<std::thread> pool;
  for (int t = 0; t < writers; ++t) {
    pool.emplace_back([&, t] {
      uint64_t i = static_cast<uint64_t>(t) * 1000003;
      lsm::WriteOptions wo;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string k = MakeKey(i % (c.n > 0 ? c.n : 1));
        const std::string v = MakeValue(i, c.value_size);
        const auto t0 = std::chrono::steady_clock::now();
        lsm::Status ws = db->Put(wo, k, v);
        const auto t1 = std::chrono::steady_clock::now();
        put_hist.Record(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        if (!ws.ok()) {
          errors.fetch_add(1);
        } else {
          puts.fetch_add(1, std::memory_order_relaxed);
        }
        i += static_cast<uint64_t>(writers);
      }
    });
  }
  for (int t = 0; t < readers; ++t) {
    pool.emplace_back([&, t] {
      uint64_t i = static_cast<uint64_t>(t) * 7919;
      lsm::ReadOptions ro;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string k = MakeKey(i % (c.n > 0 ? c.n : 1));
        std::string v;
        const auto t0 = std::chrono::steady_clock::now();
        lsm::Status rs = db->Get(ro, k, &v);
        const auto t1 = std::chrono::steady_clock::now();
        get_hist.Record(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        if (!rs.ok() && !rs.IsNotFound()) errors.fetch_add(1);
        gets.fetch_add(1, std::memory_order_relaxed);
        i += static_cast<uint64_t>(readers);
      }
    });
  }

  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(c.duration));
  const bool compaction_running = db->GetStats().compactions > 0;
  if (c.close_mid) {
    std::printf("close_requested_at=%.1fs  (compaction started: %s)\n",
                ElapsedSeconds(start), compaction_running ? "yes" : "no");
  }
  stop.store(true);

  const auto close_start = std::chrono::steady_clock::now();
  for (std::thread& th : pool) th.join();
  double close_sec = ElapsedSeconds(close_start);

  const lsm::Stats st = db->GetStats();
  const int rss_end_kb = [] {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return 0;
    char line[256];
    int kb = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::sscanf(line, "VmRSS: %d kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
  }();

  const double sec = ElapsedSeconds(start);
  std::printf("duration=%.1fs  threads=%d (%d writers, %d readers)\n", sec, c.threads,
              writers, readers);
  std::printf("puts=%llu (%.0f/s)  gets=%llu (%.0f/s)  errors=%llu\n",
              static_cast<unsigned long long>(puts.load()), puts.load() / sec,
              static_cast<unsigned long long>(gets.load()), gets.load() / sec,
              static_cast<unsigned long long>(errors.load()));
  std::printf("get p50=%lluus p90=%lluus p99=%lluus p999=%lluus\n",
              static_cast<unsigned long long>(get_hist.Percentile(50)),
              static_cast<unsigned long long>(get_hist.Percentile(90)),
              static_cast<unsigned long long>(get_hist.Percentile(99)),
              static_cast<unsigned long long>(get_hist.Percentile(99.9)));
  std::printf("put p50=%lluus p90=%lluus p99=%lluus p999=%lluus\n",
              static_cast<unsigned long long>(put_hist.Percentile(50)),
              static_cast<unsigned long long>(put_hist.Percentile(90)),
              static_cast<unsigned long long>(put_hist.Percentile(99)),
              static_cast<unsigned long long>(put_hist.Percentile(99.9)));
  std::printf("compactions=%llu  write_stalls=%llu\n",
              static_cast<unsigned long long>(st.compactions),
              static_cast<unsigned long long>(st.write_stalls));
  std::printf("rss_start=%dMB  rss_end=%dMB\n", rss_start_kb / 1024, rss_end_kb / 1024);
  std::printf("sst_files=%d  disk=%.1fMB\n", st.num_sst_files,
              static_cast<double>(st.disk_usage) / (1024.0 * 1024.0));

  if (c.close_mid) {
    const auto t0 = std::chrono::steady_clock::now();
    db->Close();
    close_sec = ElapsedSeconds(t0);
    std::printf("close_returned_after=%.2fs\n", close_sec);
  } else {
    db->Close();
  }
  delete db;
  return errors.load() == 0 ? 0 : 1;
}

int RunFdLeak(const Config& c) {
  const int before = CountFds();
  lsm::Options o = MakeOptions(c);
  o.write_buffer_size = 64 * 1024;  // 故意小：让每次循环都真的写 SST

  for (int it = 0; it < c.iterations; ++it) {
    lsm::DB* db = nullptr;
    lsm::Status s = lsm::DB::Open(o, &db);
    if (!s.ok()) {
      std::fprintf(stderr, "iteration %d Open: %s\n", it, s.ToString().c_str());
      return 1;
    }
    for (uint64_t i = 0; i < 200; ++i) {
      db->Put(lsm::WriteOptions(), MakeKey(i), MakeValue(i, c.value_size));
    }
    db->Flush();
    db->Close();
    delete db;
  }

  const int after = CountFds();
  std::printf("fd_before=%d  fd_after=%d  delta=%d  cycles=%d\n", before, after,
              after - before, c.iterations);
  return after - before <= 4 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  if (!ParseArgs(argc, argv, &c)) {
    Usage(argv[0]);
    return 1;
  }

  if (c.mode == "write") return RunWrite(c);
  if (c.mode == "flush") return RunFlush(c);
  if (c.mode == "read") return RunRead(c);
  if (c.mode == "quick") return RunQuick(c);
  if (c.mode == "mixed") return RunMixed(c);
  if (c.mode == "fdleak") return RunFdLeak(c);

  std::fprintf(stderr, "unknown mode: %s\n", c.mode.c_str());
  Usage(argv[0]);
  return 1;
}
