#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace columnar {

// The number of rows in one storage chunk / execution vector. See DESIGN.md
// section 2 for the cache-hierarchy reasoning, and
// benchmarks/bench_vector_size_sweep.cpp for the measured sweep: it confirms
// the qualitative claim (64 rows measurably hurts, ~13-17% slower) but does
// NOT sharply validate 2048 as uniquely optimal on this hardware -- 1024
// through 65536 all land within noise of each other. Named explicitly
// rather than overclaimed; see DESIGN.md section 2's update for the honest
// read on what the sweep did and didn't show.
inline constexpr std::size_t kVectorSize = 2048;

enum class TypeId : std::uint8_t {
  kInt32 = 0,
  kInt64 = 1,
  kDouble = 2,
  kBool = 3,
  kVarchar = 4,
};

enum class Encoding : std::uint8_t {
  kPlain = 0,
  kDictionary = 1,
  kDictionaryRle = 2,
};

constexpr const char* to_string(TypeId type) {
  switch (type) {
    case TypeId::kInt32: return "INT32";
    case TypeId::kInt64: return "INT64";
    case TypeId::kDouble: return "DOUBLE";
    case TypeId::kBool: return "BOOL";
    case TypeId::kVarchar: return "VARCHAR";
  }
  return "UNKNOWN";
}

constexpr const char* to_string(Encoding encoding) {
  switch (encoding) {
    case Encoding::kPlain: return "PLAIN";
    case Encoding::kDictionary: return "DICTIONARY";
    case Encoding::kDictionaryRle: return "DICTIONARY_RLE";
  }
  return "UNKNOWN";
}

// Maps a C++ type to its TypeId. Specialized for every fixed-width type this
// engine supports; used so templates can be instantiated per-type and still
// tag their runtime TypeId for the variant-based column chunk and the file
// format footer.
template <typename T>
struct TypeTraits;

template <>
struct TypeTraits<std::int32_t> {
  static constexpr TypeId kId = TypeId::kInt32;
};

template <>
struct TypeTraits<std::int64_t> {
  static constexpr TypeId kId = TypeId::kInt64;
};

template <>
struct TypeTraits<double> {
  static constexpr TypeId kId = TypeId::kDouble;
};

}  // namespace columnar
