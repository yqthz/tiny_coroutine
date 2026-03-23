cmake_minimum_required(VERSION 3.15)
project(TinyCoroutine
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINY_COROUTINE_ENABLE_STATS "Enable runtime statistics counters" OFF)
if(TINY_COROUTINE_ENABLE_STATS)
  add_compile_definitions(TINY_COROUTINE_ENABLE_STATS=1)
else()
  add_compile_definitions(TINY_COROUTINE_ENABLE_STATS=0)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Wall -Wextra -g -O0)  # Debug 配置
else()
  add_compile_options(-O3 -DNDEBUG)          # Release 配置
endif()

include_directories(include)           # 公共头文件目录
set(TINY_COROUTINE_CORE_SOURCES
  src/async_io.cpp
)

# 查找 liburing
find_library(URING_LIB uring)
if(NOT URING_LIB)
  message(FATAL_ERROR "liburing not found. Please install liburing-dev")
endif()

# ──────────────────────────────────────────────
# GoogleTest
# ──────────────────────────────────────────────
option(TINY_COROUTINE_TESTS "Build unit tests" ON)
if(TINY_COROUTINE_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  # Prevent GoogleTest from overriding our compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  # Add test executables under tests/
  file(GLOB TEST_SOURCES tests/*.cpp)
  if(TEST_SOURCES)
    add_executable(tiny_coroutine_tests
      ${TEST_SOURCES}
      ${TINY_COROUTINE_CORE_SOURCES}
    )
    target_link_libraries(tiny_coroutine_tests PRIVATE GTest::gtest_main ${URING_LIB})
    include(GoogleTest)
    gtest_discover_tests(tiny_coroutine_tests)
  endif()
endif()

# ──────────────────────────────────────────────
# Google Benchmark
# ──────────────────────────────────────────────
option(TINY_COROUTINE_BENCHMARKS "Build benchmarks" OFF)
if(TINY_COROUTINE_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  # Add benchmark executables under benchmarks/
  file(GLOB BENCH_SOURCES benchmarks/*.cpp)
  if(BENCH_SOURCES)
    add_executable(tiny_coroutine_benchmarks
      ${BENCH_SOURCES}
      ${TINY_COROUTINE_CORE_SOURCES}
    )
    target_link_libraries(tiny_coroutine_benchmarks PRIVATE benchmark::benchmark_main ${URING_LIB})
  endif()
endif()
