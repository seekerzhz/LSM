#include "lsm/crc32.h"

namespace lsm {

// ---------------------------------------------------------------------------
// TODO(0.4) Crc32c / Crc32cExtend
//
// 输入：任意字节串。
// 输出：32 位校验值。空输入返回 0。
// 约束：
//   - 必须实现查表版本：256 项表（多项式 0x82F63B78 的反射形式），
//     表可以在首次调用时惰性生成（用函数内 static 局部变量，C++11 起线程安全）。
//   - 初值 0xFFFFFFFF，处理完所有字节后再异或 0xFFFFFFFF。
//   - Crc32cExtend(0, data, n) 必须等于 Crc32c(data, n)。
//     注意增量语义：内部不能把 init_crc 再取反一次。
//   - 不允许调用 zlib / 系统库，要自己写位运算。
//
// 验收向量：
//   Crc32c("", 0)              == 0
//   Crc32c("123456789", 9)     == 0xE3069283
//   Crc32cExtend(0, "1234", 4) 再 extend "56789" 5 字节 == 0xE3069283
// ---------------------------------------------------------------------------

namespace {

// 供你实现时使用：生成 256 项查表
struct Crc32cTable {
  uint32_t table[256];
  Crc32cTable();
};

Crc32cTable::Crc32cTable() {
  // 占位：把表填成 0。TODO(0.4) 里你要用多项式正确生成它：
  //   for (uint32_t i = 0; i < 256; ++i) {
  //     uint32_t c = i;
  //     for (int k = 0; k < 8; ++k)
  //       c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
  //     table[i] = c;
  //   }
  for (uint32_t i = 0; i < 256; ++i) table[i] = 0;
}

const Crc32cTable& Table() {
  static const Crc32cTable t;
  return t;
}

}  // namespace

uint32_t Crc32cExtend(uint32_t init_crc, const char* data, size_t n) {
  // 占位实现：永远返回 init_crc，测试会失败
  (void)data;
  (void)n;
  (void)Table();
  return init_crc;
}

uint32_t Crc32c(const char* data, size_t n) {
  // 占位实现：永远返回 0。空输入返回 0 是正确行为，非空输入会失败。
  (void)data;
  (void)n;
  return 0;
}

}  // namespace lsm
