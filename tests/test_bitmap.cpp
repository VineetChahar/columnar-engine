#include <catch2/catch_test_macros.hpp>

#include "columnar/bitmap.hpp"

using columnar::Bitmap;

TEST_CASE("Bitmap default-constructed with all-valid", "[bitmap]") {
  Bitmap bm(10, true);
  REQUIRE(bm.size() == 10);
  REQUIRE(bm.count_set() == 10);
  REQUIRE(bm.count_null() == 0);
}

TEST_CASE("Bitmap default-constructed with all-null", "[bitmap]") {
  Bitmap bm(10, false);
  REQUIRE(bm.count_set() == 0);
  REQUIRE(bm.count_null() == 10);
}

TEST_CASE("Bitmap set/test individual bits", "[bitmap]") {
  Bitmap bm(16, true);
  bm.set(3, false);
  bm.set(15, false);
  REQUIRE_FALSE(bm.test(3));
  REQUIRE_FALSE(bm.test(15));
  REQUIRE(bm.test(0));
  REQUIRE(bm.count_set() == 14);
}

TEST_CASE("Bitmap tail padding bits beyond size don't pollute count_set", "[bitmap]") {
  // 5 bits requested; the byte holds 8. The 3 padding bits must not count.
  Bitmap bm(5, true);
  REQUIRE(bm.count_set() == 5);
}

TEST_CASE("Bitmap handles sizes that aren't multiples of 8", "[bitmap]") {
  Bitmap bm(13, false);
  bm.set(12, true);
  bm.set(0, true);
  REQUIRE(bm.count_set() == 2);
  REQUIRE(bm.test(12));
  REQUIRE_FALSE(bm.test(11));
}

TEST_CASE("Bitmap size zero is well-defined", "[bitmap]") {
  Bitmap bm;
  REQUIRE(bm.size() == 0);
  REQUIRE(bm.count_set() == 0);
}
