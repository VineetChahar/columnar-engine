#include <catch2/catch_test_macros.hpp>

#include "columnar/zone_map.hpp"

using columnar::StringZoneMap;
using columnar::ZoneMap;

TEST_CASE("ZoneMap<int> tracks min/max/null_count over observations", "[zone_map]") {
  ZoneMap<int> zm;
  zm.observe(5);
  zm.observe(1);
  zm.observe(9);
  zm.observe_null();
  REQUIRE(zm.has_values);
  REQUIRE(zm.min == 1);
  REQUIRE(zm.max == 9);
  REQUIRE(zm.null_count == 1);
}

TEST_CASE("ZoneMap with no observations has has_values == false", "[zone_map]") {
  ZoneMap<double> zm;
  zm.observe_null();
  zm.observe_null();
  REQUIRE_FALSE(zm.has_values);
  REQUIRE(zm.null_count == 2);
}

TEST_CASE("ZoneMap::can_contain prunes values outside [min, max]", "[zone_map]") {
  ZoneMap<int> zm;
  zm.observe(10);
  zm.observe(20);
  REQUIRE(zm.can_contain(15));
  REQUIRE(zm.can_contain(10));
  REQUIRE(zm.can_contain(20));
  REQUIRE_FALSE(zm.can_contain(9));
  REQUIRE_FALSE(zm.can_contain(21));
}

TEST_CASE("ZoneMap::can_contain is vacuously true when there are no values",
          "[zone_map]") {
  ZoneMap<int> zm;
  REQUIRE(zm.can_contain(12345));
}

TEST_CASE("StringZoneMap tracks lexicographic min/max", "[zone_map]") {
  StringZoneMap zm;
  zm.observe("banana");
  zm.observe("apple");
  zm.observe("cherry");
  zm.observe_null();
  REQUIRE(zm.min == "apple");
  REQUIRE(zm.max == "cherry");
  REQUIRE(zm.null_count == 1);
}
