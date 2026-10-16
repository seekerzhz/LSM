#include "lsm/crc32.h"

#include <cstdint>
#include <string>

#include "tests/test_macros.h"

namespace lsm {
namespace {

TEST(Crc32, EmptyIsZero) {
  EXPECT_EQ(0u, Crc32c("", 0));
}

TEST(Crc32, KnownVector) {
  // CRC32C 的标准检验值："123456789" -> 0xE3069283
  const char* s = "123456789";
  EXPECT_EQ(0xE3069283u, Crc32c(s, 9));
}

TEST(Crc32, IncrementalMatchesOneShot) {
  const std::string data = "The quick brown fox jumps over the lazy dog";
  const uint32_t one_shot = Crc32c(data.data(), data.size());

  // 分三段增量算，结果必须相同
  uint32_t crc = Crc32cExtend(0, data.data(), 10);
  crc = Crc32cExtend(crc, data.data() + 10, 10);
  crc = Crc32cExtend(crc, data.data() + 20, data.size() - 20);
  EXPECT_EQ(one_shot, crc);
}

TEST(Crc32, DetectsSingleBitFlip) {
  std::string a(1024, 'x');
  std::string b = a;
  b[500] ^= 0x01;
  EXPECT_NE(Crc32c(a.data(), a.size()), Crc32c(b.data(), b.size()));
}

TEST(Crc32, SliceOverload) {
  const std::string s = "abc";
  EXPECT_EQ(Crc32c(s.data(), s.size()), Crc32c(Slice(s)));
}

}  // namespace
}  // namespace lsm
