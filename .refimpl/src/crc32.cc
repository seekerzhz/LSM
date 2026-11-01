// 参考实现（教师版），仅用于验证骨架。
#include "lsm/crc32.h"

namespace lsm {
namespace {

struct Crc32cTable {
  uint32_t table[256];
  Crc32cTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
  }
};

const Crc32cTable& Table() {
  static const Crc32cTable t;
  return t;
}

}  // namespace

uint32_t Crc32cExtend(uint32_t init_crc, const char* data, size_t n) {
  const Crc32cTable& t = Table();
  uint32_t crc = init_crc ^ 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t byte = static_cast<uint8_t>(data[i]);
    crc = t.table[(crc ^ byte) & 0xff] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

uint32_t Crc32c(const char* data, size_t n) {
  return Crc32cExtend(0, data, n);
}

}  // namespace lsm
