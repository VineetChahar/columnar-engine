#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "columnar/column_chunk.hpp"

using namespace columnar;

// NB throughout: validity masks use std::array<bool, N> (or a C array), not
// std::vector<bool> -- vector<bool> is a bit-packed specialization, not a
// real contiguous array of bool, and can't bind to std::span<const bool>.

TEST_CASE("PlainColumnChunk<int32_t> stores values and nulls", "[column_chunk]") {
  std::vector<std::int32_t> values = {10, 20, 30, 40, 50};
  std::array<bool, 5> validity = {true, true, false, true, true};
  PlainColumnChunk<std::int32_t> chunk(values, validity);

  REQUIRE(chunk.row_count() == 5);
  REQUIRE(chunk.value(0) == 10);
  REQUIRE(chunk.is_null(2));
  REQUIRE_FALSE(chunk.is_null(0));
  REQUIRE(chunk.zone_map().min == 10);
  REQUIRE(chunk.zone_map().max == 50);
  REQUIRE(chunk.zone_map().null_count == 1);
  REQUIRE(chunk.zone_map().row_count == 5);
}

TEST_CASE("PlainColumnChunk<double> zone map ignores null values", "[column_chunk]") {
  std::vector<double> values = {1.5, 1000.0, 2.5};
  std::array<bool, 3> validity = {true, false, true};
  PlainColumnChunk<double> chunk(values, validity);
  // The null at index 1 holds 1000.0 but must not affect min/max.
  REQUIRE(chunk.zone_map().min == 1.5);
  REQUIRE(chunk.zone_map().max == 2.5);
}

TEST_CASE("PlainColumnChunk all-null column has has_values == false", "[column_chunk]") {
  std::vector<std::int64_t> values = {1, 2, 3};
  std::array<bool, 3> validity = {false, false, false};
  PlainColumnChunk<std::int64_t> chunk(values, validity);
  REQUIRE_FALSE(chunk.zone_map().has_values);
  REQUIRE(chunk.zone_map().null_count == 3);
}

TEST_CASE("BoolColumnChunk stores packed bits and validity separately",
          "[column_chunk]") {
  std::array<bool, 4> values = {true, false, true, true};
  std::array<bool, 4> validity = {true, true, false, true};
  BoolColumnChunk chunk(values, validity);
  REQUIRE(chunk.value(0));
  REQUIRE_FALSE(chunk.value(1));
  REQUIRE(chunk.is_null(2));
  REQUIRE(chunk.value(3));
}

TEST_CASE("VarcharColumnChunk stores variable-length strings contiguously",
          "[column_chunk]") {
  std::vector<std::string_view> values = {"alpha", "b", "", "delta"};
  std::array<bool, 4> validity = {true, true, false, true};
  VarcharColumnChunk chunk(values, validity);

  REQUIRE(chunk.row_count() == 4);
  REQUIRE(chunk.value(0) == "alpha");
  REQUIRE(chunk.value(1) == "b");
  REQUIRE(chunk.is_null(2));
  REQUIRE(chunk.value(3) == "delta");
  REQUIRE(chunk.zone_map().min == "alpha");
  REQUIRE(chunk.zone_map().max == "delta");
}

TEST_CASE("VarcharColumnChunk with all-empty strings has zero data bytes",
          "[column_chunk]") {
  std::vector<std::string_view> values = {"", "", ""};
  std::array<bool, 3> validity = {true, true, true};
  VarcharColumnChunk chunk(values, validity);
  REQUIRE(chunk.data_bytes() == 0);
  REQUIRE(chunk.value(0).empty());
}

TEST_CASE("DictionaryColumnChunk deduplicates repeated values", "[column_chunk]") {
  std::vector<std::string_view> values = {"US", "IN", "US", "US", "IN", "DE"};
  std::array<bool, 6> validity;
  validity.fill(true);
  DictionaryColumnChunk chunk(values, validity);

  REQUIRE(chunk.dictionary_size() == 3);
  REQUIRE(chunk.value(0) == "US");
  REQUIRE(chunk.value(1) == "IN");
  REQUIRE(chunk.value(5) == "DE");
  REQUIRE(chunk.code(0) == chunk.code(2));
  REQUIRE(chunk.code(2) == chunk.code(3));
  REQUIRE(chunk.code(0) != chunk.code(1));
}

TEST_CASE("DictionaryColumnChunk handles nulls without adding a dictionary entry",
          "[column_chunk]") {
  std::vector<std::string_view> values = {"US", "IGNORED", "US"};
  std::array<bool, 3> validity = {true, false, true};
  DictionaryColumnChunk chunk(values, validity);
  REQUIRE(chunk.dictionary_size() == 1);
  REQUIRE(chunk.is_null(1));
  REQUIRE(chunk.zone_map().null_count == 1);
}

TEST_CASE("DictionaryRleColumnChunk groups consecutive equal values into runs",
          "[column_chunk]") {
  std::vector<std::string_view> values = {"DE", "DE", "DE", "IN", "IN", "US"};
  std::array<bool, 6> validity;
  validity.fill(true);
  DictionaryRleColumnChunk chunk(values, validity);

  REQUIRE(chunk.run_count() == 3);
  REQUIRE(chunk.runs()[0].length == 3);
  REQUIRE(chunk.runs()[1].length == 2);
  REQUIRE(chunk.runs()[2].length == 1);
  REQUIRE(chunk.value(0) == "DE");
  REQUIRE(chunk.value(2) == "DE");
  REQUIRE(chunk.value(3) == "IN");
  REQUIRE(chunk.value(5) == "US");
}

TEST_CASE("DictionaryRleColumnChunk: a null in the middle of a run splits it",
          "[column_chunk]") {
  std::vector<std::string_view> values = {"A", "A", "IGNORED", "A", "A"};
  std::array<bool, 5> validity = {true, true, false, true, true};
  DictionaryRleColumnChunk chunk(values, validity);

  // Without the null, this would be one run of length 5; the null forces
  // three runs: "A"x2, null x1, "A"x2.
  REQUIRE(chunk.run_count() == 3);
  REQUIRE(chunk.runs()[0].length == 2);
  REQUIRE_FALSE(chunk.runs()[0].is_null);
  REQUIRE(chunk.runs()[1].length == 1);
  REQUIRE(chunk.runs()[1].is_null);
  REQUIRE(chunk.runs()[2].length == 2);
  REQUIRE_FALSE(chunk.runs()[2].is_null);
  REQUIRE(chunk.is_null(2));
  REQUIRE_FALSE(chunk.is_null(0));
}

TEST_CASE("ColumnChunkVariant free functions dispatch correctly", "[column_chunk]") {
  std::vector<std::int32_t> values = {1, 2, 3};
  std::array<bool, 3> validity = {true, false, true};
  ColumnChunkVariant chunk = PlainColumnChunk<std::int32_t>(values, validity);

  REQUIRE(row_count(chunk) == 3);
  REQUIRE(null_count(chunk) == 1);
  REQUIRE(type_id(chunk) == TypeId::kInt32);
  REQUIRE(encoding(chunk) == Encoding::kPlain);
}
