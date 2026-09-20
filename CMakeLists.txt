cmake_minimum_required(VERSION 3.20)
project(columnar_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(COLUMNAR_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(COLUMNAR_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(COLUMNAR_BUILD_TESTS "Build Catch2 tests" ON)
option(COLUMNAR_BUILD_BENCHMARKS "Build Google Benchmark suite" ON)

add_library(columnar_warnings INTERFACE)
target_compile_options(columnar_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
  -Werror
)

add_library(columnar_sanitizers INTERFACE)
if(COLUMNAR_ENABLE_ASAN OR COLUMNAR_ENABLE_UBSAN)
  set(_sanitizer_flags "")
  if(COLUMNAR_ENABLE_ASAN)
    list(APPEND _sanitizer_flags -fsanitize=address)
  endif()
  if(COLUMNAR_ENABLE_UBSAN)
    list(APPEND _sanitizer_flags -fsanitize=undefined)
  endif()
  target_compile_options(columnar_sanitizers INTERFACE ${_sanitizer_flags} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(columnar_sanitizers INTERFACE ${_sanitizer_flags})
endif()

add_subdirectory(src)

if(COLUMNAR_BUILD_TESTS OR COLUMNAR_BUILD_BENCHMARKS)
  include(FetchContent)
endif()

if(COLUMNAR_BUILD_TESTS)
  FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
  )
  FetchContent_MakeAvailable(catch2)
  enable_testing()
  add_subdirectory(tests)
endif()

if(COLUMNAR_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
  )
  FetchContent_MakeAvailable(googlebenchmark)
  add_subdirectory(benchmarks)
endif()
