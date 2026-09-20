#include <catch2/catch_test_macros.hpp>

#include "columnar/aligned_buffer.hpp"

using columnar::AlignedBuffer;

TEST_CASE("AlignedBuffer allocates and is actually aligned", "[aligned_buffer]") {
  AlignedBuffer<int, 64> buf(100);
  REQUIRE(buf.size() == 100);
  REQUIRE(reinterpret_cast<std::uintptr_t>(buf.data()) % 64 == 0);
  for (int i = 0; i < 100; ++i) {
    buf[i] = i;
  }
  for (int i = 0; i < 100; ++i) {
    REQUIRE(buf[i] == i);
  }
}

TEST_CASE("AlignedBuffer with size zero does not allocate", "[aligned_buffer]") {
  AlignedBuffer<int> buf(0);
  REQUIRE(buf.size() == 0);
  REQUIRE(buf.empty());
  REQUIRE(buf.data() == nullptr);
}

TEST_CASE("AlignedBuffer move constructor transfers ownership", "[aligned_buffer]") {
  AlignedBuffer<int> a(10);
  a[0] = 42;
  int* original_ptr = a.data();

  AlignedBuffer<int> b(std::move(a));
  REQUIRE(b.data() == original_ptr);
  REQUIRE(b[0] == 42);
  REQUIRE(a.data() == nullptr);   // moved-from
  REQUIRE(a.size() == 0);
}

TEST_CASE("AlignedBuffer move assignment releases prior contents", "[aligned_buffer]") {
  AlignedBuffer<int> a(10);
  AlignedBuffer<int> b(5);
  b[0] = 7;
  int* b_ptr = b.data();

  a = std::move(b);
  REQUIRE(a.data() == b_ptr);
  REQUIRE(a.size() == 5);
  REQUIRE(a[0] == 7);
}
