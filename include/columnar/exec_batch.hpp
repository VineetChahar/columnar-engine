#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace columnar {

// The executor's runtime value types -- deliberately narrower than storage
// TypeId. int32 columns get promoted to int64 the moment they're scanned
// off disk, so every arithmetic/comparison path in the expression evaluator
// only ever has to handle one integer width. This is the same call SQLite
// makes internally (everything is a 64-bit int, a double, or text) and for
// the same reason: it collapses a type-combinatorics problem in exchange
// for a promotion that's free on any 64-bit machine.
enum class ExecType { kInt64, kDouble, kBool, kText };

// One column's worth of a batch: a dense, densely-packed value array plus a
// side validity array (1 = valid), same "data separate from nulls" idea as
// the storage layer's Bitmap, just byte-per-row instead of bit-per-row --
// simpler to index/build for ephemeral, short-lived execution batches where
// the 8x memory cost doesn't matter as much as it would at rest.
//
// Text columns own std::strings here (one heap allocation per distinct
// string, not per row -- see LogicalPlan/operators.cpp for how dictionary
// columns get shared instead of copied where it matters). This is a real,
// deliberate departure from the storage layer's zero-per-row-allocation
// discipline: intermediate execution results are short-lived and rebuilt
// batch-by-batch, so the engineering cost of an offset+buffer
// representation for *ephemeral* data buys a lot less than it does for data
// at rest. See LEARNING.md.
struct ExecColumn {
  ExecType type = ExecType::kInt64;
  std::variant<std::vector<std::int64_t>, std::vector<double>, std::vector<std::uint8_t>,
               std::vector<std::string>>
      data;
  std::vector<std::uint8_t> validity;  // 1 = valid; validity.size() == row count

  std::size_t size() const noexcept { return validity.size(); }

  std::vector<std::int64_t>& ints() { return std::get<std::vector<std::int64_t>>(data); }
  const std::vector<std::int64_t>& ints() const {
    return std::get<std::vector<std::int64_t>>(data);
  }
  std::vector<double>& doubles() { return std::get<std::vector<double>>(data); }
  const std::vector<double>& doubles() const { return std::get<std::vector<double>>(data); }
  std::vector<std::uint8_t>& bools() { return std::get<std::vector<std::uint8_t>>(data); }
  const std::vector<std::uint8_t>& bools() const {
    return std::get<std::vector<std::uint8_t>>(data);
  }
  std::vector<std::string>& texts() { return std::get<std::vector<std::string>>(data); }
  const std::vector<std::string>& texts() const {
    return std::get<std::vector<std::string>>(data);
  }

  static ExecColumn make(ExecType t, std::size_t n) {
    ExecColumn col;
    col.type = t;
    switch (t) {
      case ExecType::kInt64: col.data = std::vector<std::int64_t>(n); break;
      case ExecType::kDouble: col.data = std::vector<double>(n); break;
      case ExecType::kBool: col.data = std::vector<std::uint8_t>(n); break;
      case ExecType::kText: col.data = std::vector<std::string>(n); break;
    }
    col.validity.assign(n, 1);
    return col;
  }
};

// A named batch of up to kVectorSize rows -- the unit every operator in
// Phase 3 passes between next() calls. Names travel with the batch (rather
// than being looked up via a separate schema object) so operators can be
// tested and composed without a surrounding plan.
struct ExecBatch {
  std::vector<std::string> column_names;
  std::vector<ExecColumn> columns;

  std::size_t row_count() const noexcept {
    return columns.empty() ? 0 : columns[0].size();
  }

  std::size_t column_index(std::string_view name) const {
    for (std::size_t i = 0; i < column_names.size(); ++i) {
      if (column_names[i] == name) return i;
    }
    return static_cast<std::size_t>(-1);
  }
};

}  // namespace columnar
