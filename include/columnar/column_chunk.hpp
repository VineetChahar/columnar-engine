#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "columnar/aligned_buffer.hpp"
#include "columnar/bitmap.hpp"
#include "columnar/types.hpp"
#include "columnar/zone_map.hpp"

namespace columnar {

// A dense, fixed-width column chunk: no encoding beyond "store the values
// contiguously and mark nulls in a side bitmap." This is the baseline every
// encoded representation (dictionary, RLE) is benchmarked against.
//
// T is explicitly instantiated for int32_t/int64_t/double in
// column_chunk.cpp -- the definition lives out-of-line because there are
// only three concrete instantiations we ever need, so there's no reason to
// force every translation unit that includes this header to re-parse (and
// re-instantiate) the implementation.
template <typename T>
class PlainColumnChunk {
 public:
  static_assert(std::is_arithmetic_v<T>, "PlainColumnChunk is for numeric types");
  static constexpr TypeId kTypeId = TypeTraits<T>::kId;
  static constexpr Encoding kEncoding = Encoding::kPlain;

  // values.size() == validity.size() == row_count, and row_count must be
  // <= kVectorSize. validity[i] == false marks row i as null; values[i] is
  // ignored (but still read) in that case.
  PlainColumnChunk(std::span<const T> values, std::span<const bool> validity);

  // Tag-dispatched constructor used by the file reader (file_format.cpp) to
  // reconstruct a chunk directly from bytes already decoded off disk,
  // without re-deriving the zone map by replaying every value through
  // observe() a second time.
  struct RawPartsTag {};
  PlainColumnChunk(RawPartsTag, AlignedBuffer<T> values, Bitmap validity,
                    ZoneMap<T> zone_map, std::size_t row_count)
      : values_(std::move(values)),
        validity_(std::move(validity)),
        zone_map_(zone_map),
        row_count_(row_count) {}

  std::size_t row_count() const noexcept { return row_count_; }
  bool is_null(std::size_t i) const noexcept { return !validity_.test(i); }
  const T& value(std::size_t i) const noexcept { return values_[i]; }
  std::span<const T> values() const noexcept { return {values_.data(), row_count_}; }
  const Bitmap& validity() const noexcept { return validity_; }
  const ZoneMap<T>& zone_map() const noexcept { return zone_map_; }

 private:
  AlignedBuffer<T> values_;
  Bitmap validity_;
  ZoneMap<T> zone_map_;
  std::size_t row_count_ = 0;
};

// Bit-packed bool column: the data itself is a Bitmap (1 bit/value), stored
// separately from the validity Bitmap (1 bit/row, is-this-row-null). Two
// bitmaps rather than a tri-state encoding because it keeps "is this row
// null" answerable the same way for every column type in the engine.
class BoolColumnChunk {
 public:
  static constexpr TypeId kTypeId = TypeId::kBool;
  static constexpr Encoding kEncoding = Encoding::kPlain;

  BoolColumnChunk(std::span<const bool> values, std::span<const bool> validity);

  struct RawPartsTag {};
  BoolColumnChunk(RawPartsTag, Bitmap data, Bitmap validity, ZoneMap<bool> zone_map,
                   std::size_t row_count)
      : data_(std::move(data)),
        validity_(std::move(validity)),
        zone_map_(zone_map),
        row_count_(row_count) {}

  std::size_t row_count() const noexcept { return row_count_; }
  bool is_null(std::size_t i) const noexcept { return !validity_.test(i); }
  bool value(std::size_t i) const noexcept { return data_.test(i); }
  const Bitmap& data_bitmap() const noexcept { return data_; }
  const Bitmap& validity() const noexcept { return validity_; }
  const ZoneMap<bool>& zone_map() const noexcept { return zone_map_; }

 private:
  Bitmap data_;
  Bitmap validity_;
  ZoneMap<bool> zone_map_;
  std::size_t row_count_ = 0;
};

// General-purpose string column: a dense offsets array (row_count + 1
// entries) plus one contiguous byte buffer, Arrow-style. No per-row heap
// allocation, and the buffer is mmap-able as-is. See DESIGN.md section 4.
class VarcharColumnChunk {
 public:
  static constexpr TypeId kTypeId = TypeId::kVarchar;
  static constexpr Encoding kEncoding = Encoding::kPlain;

  VarcharColumnChunk(std::span<const std::string_view> values,
                      std::span<const bool> validity);

  struct RawPartsTag {};
  VarcharColumnChunk(RawPartsTag, AlignedBuffer<std::uint32_t> offsets,
                      AlignedBuffer<char> data, Bitmap validity,
                      StringZoneMap zone_map, std::size_t row_count)
      : offsets_(std::move(offsets)),
        data_(std::move(data)),
        validity_(std::move(validity)),
        zone_map_(std::move(zone_map)),
        row_count_(row_count) {}

  std::size_t row_count() const noexcept { return row_count_; }
  bool is_null(std::size_t i) const noexcept { return !validity_.test(i); }
  std::string_view value(std::size_t i) const noexcept {
    return {data_.data() + offsets_[i], offsets_[i + 1] - offsets_[i]};
  }
  const Bitmap& validity() const noexcept { return validity_; }
  const StringZoneMap& zone_map() const noexcept { return zone_map_; }
  std::size_t data_bytes() const noexcept { return data_.size(); }
  std::span<const std::uint32_t> offsets() const noexcept {
    return {offsets_.data(), row_count_ + 1};
  }
  const char* raw_data() const noexcept { return data_.data(); }

 private:
  AlignedBuffer<std::uint32_t> offsets_;
  AlignedBuffer<char> data_;
  Bitmap validity_;
  StringZoneMap zone_map_;
  std::size_t row_count_ = 0;
};

// Low-cardinality string column: a small dictionary of unique values plus a
// dense per-row array of integer codes indexing into it. Turns string
// comparison/grouping into integer comparison/grouping. See DESIGN.md
// section 4 -- and benchmarks/bench_encoding.cpp for where this stops
// paying for itself as cardinality rises.
class DictionaryColumnChunk {
 public:
  static constexpr TypeId kTypeId = TypeId::kVarchar;
  static constexpr Encoding kEncoding = Encoding::kDictionary;

  DictionaryColumnChunk(std::span<const std::string_view> values,
                         std::span<const bool> validity);

  struct RawPartsTag {};
  DictionaryColumnChunk(RawPartsTag, std::vector<std::string> dictionary,
                         AlignedBuffer<std::uint32_t> codes, Bitmap validity,
                         StringZoneMap zone_map, std::size_t row_count)
      : dictionary_(std::move(dictionary)),
        codes_(std::move(codes)),
        validity_(std::move(validity)),
        zone_map_(std::move(zone_map)),
        row_count_(row_count) {}

  std::size_t row_count() const noexcept { return row_count_; }
  bool is_null(std::size_t i) const noexcept { return !validity_.test(i); }
  std::string_view value(std::size_t i) const noexcept {
    return dictionary_[codes_[i]];
  }
  std::uint32_t code(std::size_t i) const noexcept { return codes_[i]; }
  std::size_t dictionary_size() const noexcept { return dictionary_.size(); }
  std::string_view dictionary_entry(std::uint32_t code) const noexcept {
    return dictionary_[code];
  }
  const std::vector<std::string>& dictionary() const noexcept { return dictionary_; }
  std::span<const std::uint32_t> codes() const noexcept {
    return {codes_.data(), row_count_};
  }
  const Bitmap& validity() const noexcept { return validity_; }
  const StringZoneMap& zone_map() const noexcept { return zone_map_; }

 private:
  std::vector<std::string> dictionary_;  // ordinal position == code
  AlignedBuffer<std::uint32_t> codes_;
  Bitmap validity_;
  StringZoneMap zone_map_;
  std::size_t row_count_ = 0;
};

// One run of a run-length-encoded dictionary column: `length` consecutive
// rows all equal to `code` (or all null, if `is_null` -- a null run never
// carries a meaningful code). Kept as its own struct, rather than reusing
// DictionaryColumnChunk's per-row codes_, because a null in the middle of an
// otherwise-repeating column legitimately ends a run -- see LEARNING.md.
struct RleRun {
  std::uint32_t code = 0;
  std::uint32_t length = 0;
  bool is_null = false;
};

// Dictionary encoding plus run-length encoding of the codes: pays off when
// the column is sorted or naturally clustered (see benchmarks/bench_encoding.cpp).
class DictionaryRleColumnChunk {
 public:
  static constexpr TypeId kTypeId = TypeId::kVarchar;
  static constexpr Encoding kEncoding = Encoding::kDictionaryRle;

  DictionaryRleColumnChunk(std::span<const std::string_view> values,
                            std::span<const bool> validity);

  struct RawPartsTag {};
  DictionaryRleColumnChunk(RawPartsTag, std::vector<std::string> dictionary,
                            std::vector<RleRun> runs, StringZoneMap zone_map,
                            std::size_t row_count)
      : dictionary_(std::move(dictionary)),
        runs_(std::move(runs)),
        zone_map_(std::move(zone_map)),
        row_count_(row_count) {}

  std::size_t row_count() const noexcept { return row_count_; }
  std::size_t run_count() const noexcept { return runs_.size(); }
  const std::vector<RleRun>& runs() const noexcept { return runs_; }
  std::size_t dictionary_size() const noexcept { return dictionary_.size(); }
  std::string_view dictionary_entry(std::uint32_t code) const noexcept {
    return dictionary_[code];
  }
  const std::vector<std::string>& dictionary() const noexcept { return dictionary_; }
  const StringZoneMap& zone_map() const noexcept { return zone_map_; }

  bool is_null(std::size_t i) const noexcept;
  // O(run_count()), not O(1): walks runs to find which one contains row i.
  // Fine for spot checks/tests; a real scan operator should iterate runs
  // directly instead of calling value(i) per row -- that's the whole point
  // of RLE.
  std::string_view value(std::size_t i) const;

  // STUB -- implement me (see LEARNING.md / quiz for Phase 1).
  //
  // Expand runs_ back into one entry per row: dictionary code for a valid
  // row, std::nullopt for a null row. This is the function that determines
  // whether RLE is "free" for a downstream consumer that isn't run-aware --
  // if decode dominates the runtime, RLE only helps operators written to
  // consume runs directly (which is what the hash-aggregate/filter
  // operators in Phase 3 should do).
  //
  // Spec:
  //  - Returned vector.size() == row_count().
  //  - For row i: result[i] == std::nullopt iff is_null(i); otherwise
  //    result[i] == code of that row (matches what code(i) would report on
  //    an equivalent DictionaryColumnChunk).
  std::vector<std::optional<std::uint32_t>> decode_to_codes() const;

 private:
  std::vector<std::string> dictionary_;
  std::vector<RleRun> runs_;
  StringZoneMap zone_map_;
  std::size_t row_count_ = 0;
};

using ColumnChunkVariant =
    std::variant<PlainColumnChunk<std::int32_t>, PlainColumnChunk<std::int64_t>,
                 PlainColumnChunk<double>, BoolColumnChunk, VarcharColumnChunk,
                 DictionaryColumnChunk, DictionaryRleColumnChunk>;

std::size_t row_count(const ColumnChunkVariant& chunk);
std::uint32_t null_count(const ColumnChunkVariant& chunk);
TypeId type_id(const ColumnChunkVariant& chunk);
Encoding encoding(const ColumnChunkVariant& chunk);

}  // namespace columnar
