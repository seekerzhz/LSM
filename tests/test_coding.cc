#include "lsm/coding.h"

#include <cstdint>
#include <string>

#include "tests/test_macros.h"

namespace lsm {
namespace {

TEST(Coding, Varint32RoundTrip) {
  const uint32_t cases[] = {0,    1,      127,     128,      300,      16383,
                            16384, 2097151, 2097152, 268435455, 268435456,
                            4294967295u};
  for (uint32_t v : cases) {
    char buf[10];
    const int n = PutVarint32(buf, v);
    EXPECT_TRUE(n >= 1 && n <= 5);
    Slice input(buf, static_cast<size_t>(n));
    uint32_t out = 0;
    ASSERT_TRUE(GetVarint32(&input, &out));
    EXPECT_EQ(v, out);
    EXPECT_TRUE(input.empty());
  }
}

TEST(Coding, Varint32KnownValues) {
  struct Case {
    uint32_t v;
    const char* bytes;
    int len;
  };
  const Case cases[] = {
      {0, "\x00", 1},
      {1, "\x01", 1},
      {127, "\x7f", 1},
      {128, "\x80\x01", 2},
      {300, "\xac\x02", 2},
      {16383, "\xff\x7f", 2},
      {16384, "\x80\x80\x01", 3},
      {4294967295u, "\xff\xff\xff\xff\x0f", 5},
  };
  for (const auto& c : cases) {
    char buf[10];
    const int n = PutVarint32(buf, c.v);
    EXPECT_EQ(c.len, n);
    EXPECT_TRUE(std::string(buf, static_cast<size_t>(n)) ==
                std::string(c.bytes, static_cast<size_t>(c.len)));
    EXPECT_EQ(c.len, VarintLength(c.v));
  }
}

TEST(Coding, Varint32Malformed) {
  // 1. 空输入
  {
    Slice s;
    uint32_t v = 0;
    EXPECT_FALSE(GetVarint32(&s, &v));
  }
  // 2. 全是续位标志，超过 5 字节仍无终止
  {
    const char buf[] = "\xff\xff\xff\xff\xff\xff";
    Slice s(buf, sizeof(buf) - 1);
    uint32_t v = 0;
    EXPECT_FALSE(GetVarint32(&s, &v));
  }
  // 3. 声明需要 2 字节但只剩 1 字节
  {
    const char buf[] = "\x80";
    Slice s(buf, 1);
    uint32_t v = 0;
    EXPECT_FALSE(GetVarint32(&s, &v));
  }
}

TEST(Coding, Varint64Large) {
  const uint64_t vals[] = {0, 1, 300, 1ull << 35, 0xffffffffull,
                           (1ull << 56) - 1, ~0ull};
  for (uint64_t v : vals) {
    char buf[16];
    const int n = PutVarint64(buf, v);
    Slice input(buf, static_cast<size_t>(n));
    uint64_t out = 0;
    ASSERT_TRUE(GetVarint64(&input, &out));
    EXPECT_EQ(v, out);
  }
}

TEST(Coding, FixedRoundTrip) {
  char buf[8];
  EncodeFixed32(buf, 0x12345678u);
  EXPECT_EQ(0x12345678u, DecodeFixed32(buf));
  // 小端：最低字节在前
  EXPECT_EQ(0x78, static_cast<int>(static_cast<unsigned char>(buf[0])));
  EncodeFixed64(buf, 0x1122334455667788ull);
  EXPECT_EQ(0x1122334455667788ull, DecodeFixed64(buf));
}

TEST(Coding, LengthPrefixedSlice) {
  std::string dst;
  PutLengthPrefixedSlice(&dst, Slice("hello"));
  PutLengthPrefixedSlice(&dst, Slice(""));
  PutLengthPrefixedSlice(&dst, Slice("worldworldworld"));

  Slice input(dst);
  Slice a, b, c;
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &a));
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &b));
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &c));
  EXPECT_TRUE(a == Slice("hello"));
  EXPECT_TRUE(b.empty());
  EXPECT_TRUE(c == Slice("worldworldworld"));
  EXPECT_TRUE(input.empty());
}

TEST(Coding, LengthPrefixedRejectsOverlong) {
  // 声明长度 100，但只有 3 字节内容
  std::string dst;
  char head[5];
  const int n = PutVarint32(head, 100);
  dst.append(head, static_cast<size_t>(n));
  dst.append("abc");
  Slice input(dst);
  Slice out;
  EXPECT_FALSE(GetLengthPrefixedSlice(&input, &out));
}

}  // namespace
}  // namespace lsm
