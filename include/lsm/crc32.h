// CRC32C（Castagnoli 多项式 0x1EDC6F41），与 LevelDB / RocksDB 一致。
//
// 为什么用 CRC32C 而不是 CRC32：Castagnoli 多项式在硬件里有指令支持
// （SSE4.2 的 crc32 指令），软件实现也比 CRC32 快一点点。
#pragma once

#include <cstddef>
#include <cstdint>

#include "lsm/slice.h"

namespace lsm {

// 计算 [data, data+n) 的 CRC32C。空输入返回 0。
uint32_t Crc32c(const char* data, size_t n);

inline uint32_t Crc32c(const Slice& s) { return Crc32c(s.data(), s.size()); }

// 增量计算：把 init_crc 当作"前面那段数据的校验结果"，继续算后面的数据。
// 用于分片计算（本课程在 WAL / SST 里分块校验时用到）。
uint32_t Crc32cExtend(uint32_t init_crc, const char* data, size_t n);

}  // namespace lsm
