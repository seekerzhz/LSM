// 手工验证 VarInt 的小工具。
//
// 用法：
//   ./varint_probe 300                  打印单个值的编码/解码往返
//   ./varint_probe 300 128 1            多个值
//   ./varint_probe --dump /tmp/a.bin 300 128 1   把编码写进文件（配合 xxd 看字节）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lsm/coding.h"
#include "lsm/env.h"

using namespace lsm;  // 这是命令行工具，不是库代码，允许

namespace {

void PrintOne(uint64_t v) {
  char buf[16];
  int n;
  if (v <= 0xffffffffull) {
    n = PutVarint32(buf, static_cast<uint32_t>(v));
  } else {
    n = PutVarint64(buf, v);
  }

  std::printf("input=%-12llu encoded=[", static_cast<unsigned long long>(v));
  for (int i = 0; i < n; ++i) {
    std::printf("%02x%s", static_cast<unsigned char>(buf[i]), i + 1 < n ? " " : "");
  }
  std::printf("]  len=%d", n);

  Slice input(buf, static_cast<size_t>(n));
  uint64_t out = 0;
  bool ok;
  if (v <= 0xffffffffull) {
    uint32_t tmp = 0;
    ok = GetVarint32(&input, &tmp);
    out = tmp;
  } else {
    ok = GetVarint64(&input, &out);
  }
  std::printf("  decoded=%llu  %s\n", static_cast<unsigned long long>(out),
              (ok && out == v) ? "ok" : "MISMATCH");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s [--dump FILE] <value> [value...]\n"
                 "       %s --bits <value>      (打印每个字节的二进制)\n",
                 argv[0], argv[0]);
    return 1;
  }

  std::string dump_path;
  int i = 1;
  if (std::strcmp(argv[1], "--dump") == 0 && argc >= 3) {
    dump_path = argv[2];
    i = 3;
  }

  if (i >= argc) {
    std::fprintf(stderr, "no value given\n");
    return 1;
  }

  std::string blob;
  for (; i < argc; ++i) {
    const uint64_t v = std::strtoull(argv[i], nullptr, 10);
    PrintOne(v);
    char buf[16];
    const int n = (v <= 0xffffffffull) ? PutVarint32(buf, static_cast<uint32_t>(v))
                                       : PutVarint64(buf, v);
    blob.append(buf, static_cast<size_t>(n));
  }

  if (!dump_path.empty()) {
    std::unique_ptr<WritableFile> f;
    Status s = Env::Default()->NewWritableFile(dump_path, &f);
    if (!s.ok()) {
      std::fprintf(stderr, "open %s failed: %s\n", dump_path.c_str(),
                   s.ToString().c_str());
      return 1;
    }
    f->Append(Slice(blob));
    f->Sync();
    std::printf("wrote %zu bytes to %s\n", blob.size(), dump_path.c_str());
  }
  return 0;
}
