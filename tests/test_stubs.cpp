// Tests for the Phase 1 stub functions (see the "STUB -- implement me"
// comments in zone_map.hpp, bitmap.hpp, and column_chunk.hpp). Tagged
// "[.stub]" so they're excluded from a normal `ctest`/`columnar_tests` run
// (Catch2 hides any tag starting with a dot) -- run them explicitly with
// `columnar_tests "[.stub]"` once you've implemented the functions.

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "columnar/bitmap.hpp"
#include "columnar/column_chunk.hpp"
#include "columnar/zone_map.hpp"

using namespace columnar;

TEST_CASE("ZoneMap::merged_with combines two disjoint ranges", "[.stub][zone_map]") {
  ZoneMap<int> a;
  a.observe(5);
  a.observe(1);
  a.observe_null();

  ZoneMap<int> b;
  b.observe(10);
  b.observe(3);

  ZoneMap<int> merged = a.merged_with(b);
  REQUIRE(merged.has_values);
  REQUIRE(merged.min == 1);
  REQUIRE(merged.max == 10);
  REQUIRE(merged.null_count == 1);
  REQUIRE(merged.row_count == a.row_count + b.row_count);

  // Must not have mutated the inputs.
  REQUIRE(a.max == 5);
  REQUIRE(b.min == 3);
}

TEST_CASE("ZoneMap::merged_with when one side has no values", "[.stub][zone_map]") {
  ZoneMap<int> a;
  a.observe_null();
  a.observe_null();

  ZoneMap<int> b;
  b.observe(42);

  ZoneMap<int> merged = a.merged_with(b);
  REQUIRE(merged.has_values);
  REQUIRE(merged.min == 42);
  REQUIRE(merged.max == 42);
}

TEST_CASE("Bitmap::count_set_range counts a sub-range without a full copy",
          "[.stub][bitmap]") {
  Bitmap bm(20, false);
  for (std::size_t i = 5; i < 15; ++i) {
    bm.set(i, true);
  }
  REQUIRE(bm.count_set_range(0, 20) == 10);
  REQUIRE(bm.count_set_range(5, 10) == 10);
  REQUIRE(bm.count_set_range(0, 5) == 0);
  REQUIRE(bm.count_set_range(10, 10) == 5);   // rows [10, 20): 10..14 set, 15..19 not
  REQUIRE(bm.count_set_range(7, 4) == 4);     // rows [7, 11): all within the set range
}

TEST_CASE("DictionaryRleColumnChunk::decode_to_codes expands runs to one entry per row",
          "[.stub][column_chunk]") {
  std::vector<std::string_view> values = {"A", "A", "IGNORED", "B", "B"};
  std::array<bool, 5> validity = {true, true, false, true, true};
  DictionaryRleColumnChunk chunk(values, validity);

  auto decoded = chunk.decode_to_codes();
  REQUIRE(decoded.size() == 5);
  REQUIRE(decoded[0].has_value());
  REQUIRE(decoded[1].has_value());
  REQUIRE(decoded[0] == decoded[1]);
  REQUIRE_FALSE(decoded[2].has_value());
  REQUIRE(decoded[3].has_value());
  REQUIRE(decoded[3] == decoded[4]);
  REQUIRE(decoded[0] != decoded[3]);
}
