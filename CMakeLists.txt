cmake_minimum_required(VERSION 3.16)
project(lsm_course CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# 本课程的文件 IO 与 fsync 语义按 POSIX 讲，Windows 原生不支持
if(WIN32 AND NOT MSYS)
  message(FATAL_ERROR
    "本课程需要 POSIX 环境（open/fsync/pread）。请在 Linux、macOS 或 WSL2 上构建。")
endif()

# ---------------------------------------------------------------------------
# 核心库
# ---------------------------------------------------------------------------
add_library(lsm_core
  src/block.cc
  src/block_builder.cc
  src/bloom.cc
  src/coding.cc
  src/compaction.cc
  src/crc32.cc
  src/dbformat.cc
  src/env_posix.cc
  src/filter_block.cc
  src/format.cc
  src/iterator.cc
  src/log_reader.cc
  src/log_writer.cc
  src/memtable.cc
  src/merge_iterator.cc
  src/status.cc
  src/table.cc
  src/table_builder.cc
  src/table_cache.cc
  src/version.cc
  src/version_edit.cc
  src/version_set.cc
  src/write_batch.cc
)
target_include_directories(lsm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(lsm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lsm_core PUBLIC pthread)

# ---------------------------------------------------------------------------
# 测试：有 GoogleTest 就用，没有就用内置 assert runner
# ---------------------------------------------------------------------------
find_package(GTest QUIET)
if(GTest_FOUND)
  message(STATUS "GoogleTest found -> tests use gtest")
  set(LSM_USE_GTEST ON)
  enable_testing()
else()
  message(STATUS "GoogleTest NOT found -> tests use built-in assert runner")
  set(LSM_USE_GTEST OFF)
endif()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc)

if(LSM_USE_GTEST)
  # gtest_main 提供 main，所以要排除自带的 runner
  list(REMOVE_ITEM TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cc)
  add_executable(lsm_tests ${TEST_SOURCES})
  target_link_libraries(lsm_tests PRIVATE lsm_core GTest::gtest GTest::gtest_main)
  add_test(NAME lsm_tests COMMAND lsm_tests)
else()
  add_executable(lsm_tests ${TEST_SOURCES})
  target_link_libraries(lsm_tests PRIVATE lsm_core)
  target_compile_definitions(lsm_tests PRIVATE LSM_NO_GTEST=1)
  add_test(NAME lsm_tests COMMAND lsm_tests)
endif()

# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------
add_executable(varint_probe tools/varint_probe.cc)
target_link_libraries(varint_probe PRIVATE lsm_core)

# ---------------------------------------------------------------------------
# 三套构建的辅助信息
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS " build type : ${CMAKE_BUILD_TYPE}")
message(STATUS " tests      : ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc")
message(STATUS " ASan build : cmake -S . -B build-asan -DCMAKE_CXX_FLAGS=\"-fsanitize=address,undefined -g\"")
message(STATUS " TSan build : cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS=\"-fsanitize=thread -g -O1\"")
message(STATUS "----------------------------------------------------------")
