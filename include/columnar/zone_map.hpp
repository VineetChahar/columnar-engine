#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace columnar {

// Per-chunk statistics: min/max of the non-null values, how many rows are
// null, and how many rows total. This is what makes Phase 4's zone-map
// pruning possible ("this chunk's max is below the filter's constant, skip
// it without touching the data"), so it's built at chunk-construction time
// rather than computed on demand.
template <typename T>
struct ZoneMap {
  T min{};
  T max{};
  std::uint32_t null_count = 0;
  std::uint32_t row_count = 0;
  bool has_values = false;  // false iff every row is null (min/max unset)

  void observe(const T& value) {
    if (!has_values) {
      min = value;
      max = value;
      has_values = true;
    } else {
      min = std::min(min, value);
      max = std::max(max, value);
    }
  }

  void observe_null() { ++null_count; }

  bool can_contain(const T& probe) const {
    return !has_values || (probe >= min && probe <= max);
  }

  // STUB -- implement me (see LEARNING.md / quiz for Phase 1).
  //
  // Combine `*this` and `other`, two zone maps covering disjoint row ranges
  // of the *same* column, into the zone map for their union. Needed once
  // Phase 1's storage chunks get grouped into coarser "row groups" for
  // cheaper (but less precise) pruning.
  //
  // Spec:
  //  - Must not mutate *this or other.
  //  - result.row_count  == this->row_count + other.row_count
  //  - result.null_count == this->null_count + other.null_count
  //  - result.has_values == this->has_values || other.has_values
  //  - if both have_values: result.min/max are the min/max across both
  //    ranges (not just of the two mins/maxes -- think about why those are
  //    actually the same thing here, and when they wouldn't be).
  //  - if only one side has_values, the result equals that side's min/max.
  //  - if neither has_values, result.has_values is false and min/max are
  //    unspecified (default-constructed T is fine).
  ZoneMap<T> merged_with(const ZoneMap<T>& other) const {
    (void)other;
    throw std::logic_error(
        "ZoneMap<T>::merged_with is a Phase 1 stub -- implement it "
        "(see the spec comment above this function).");
  }
};

// Explicit specialization for std::string so callers get lexicographic
// min/max without relying on operator< meaning something sensible for every
// future T. Kept as a plain function pair (not the templated ZoneMap<T>
// above) because varchar chunks store dictionary-encoded or offset+buffer
// data, not std::string, at rest -- see column_chunk.hpp.
struct StringZoneMap {
  std::string min;
  std::string max;
  std::uint32_t null_count = 0;
  std::uint32_t row_count = 0;
  bool has_values = false;

  void observe(std::string_view value) {
    if (!has_values) {
      min.assign(value);
      max.assign(value);
      has_values = true;
    } else {
      if (value < min) min.assign(value);
      if (value > max) max.assign(value);
    }
  }

  void observe_null() { ++null_count; }
};

}  // namespace columnar
