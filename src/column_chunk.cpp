#include "columnar/column_chunk.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_map>

namespace columnar {

// ---------------------------------------------------------------- Plain ---

template <typename T>
PlainColumnChunk<T>::PlainColumnChunk(std::span<const T> values,
                                       std::span<const bool> validity)
    : values_(values.size()),
      validity_(validity.size()),
      row_count_(values.size()) {
  assert(values.size() == validity.size());
  assert(values.size() <= kVectorSize);
  zone_map_.row_count = static_cast<std::uint32_t>(row_count_);
  for (std::size_t i = 0; i < row_count_; ++i) {
    values_[i] = values[i];
    validity_.set(i, validity[i]);
    if (validity[i]) {
      zone_map_.observe(values[i]);
    } else {
      zone_map_.observe_null();
    }
  }
}

template class PlainColumnChunk<std::int32_t>;
template class PlainColumnChunk<std::int64_t>;
template class PlainColumnChunk<double>;

// ------------------------------------------------------------------ Bool --

BoolColumnChunk::BoolColumnChunk(std::span<const bool> values,
                                  std::span<const bool> validity)
    : data_(values.size(), /*default_value=*/false),
      validity_(validity.size()),
      row_count_(values.size()) {
  assert(values.size() == validity.size());
  assert(values.size() <= kVectorSize);
  zone_map_.row_count = static_cast<std::uint32_t>(row_count_);
  for (std::size_t i = 0; i < row_count_; ++i) {
    validity_.set(i, validity[i]);
    data_.set(i, values[i]);
    if (validity[i]) {
      zone_map_.observe(values[i]);
    } else {
      zone_map_.observe_null();
    }
  }
}

// --------------------------------------------------------------- Varchar --

VarcharColumnChunk::VarcharColumnChunk(std::span<const std::string_view> values,
                                        std::span<const bool> validity)
    : offsets_(values.size() + 1),
      validity_(validity.size()),
      row_count_(values.size()) {
  assert(values.size() == validity.size());
  assert(values.size() <= kVectorSize);
  zone_map_.row_count = static_cast<std::uint32_t>(row_count_);

  std::size_t total_bytes = 0;
  for (std::size_t i = 0; i < row_count_; ++i) {
    total_bytes += validity[i] ? values[i].size() : 0;
  }
  data_ = AlignedBuffer<char>(total_bytes);

  std::uint32_t cursor = 0;
  for (std::size_t i = 0; i < row_count_; ++i) {
    offsets_[i] = cursor;
    validity_.set(i, validity[i]);
    if (validity[i]) {
      std::copy(values[i].begin(), values[i].end(), data_.data() + cursor);
      cursor += static_cast<std::uint32_t>(values[i].size());
      zone_map_.observe(values[i]);
    } else {
      zone_map_.observe_null();
    }
  }
  offsets_[row_count_] = cursor;
}

// ------------------------------------------------------------ Dictionary --

DictionaryColumnChunk::DictionaryColumnChunk(std::span<const std::string_view> values,
                                              std::span<const bool> validity)
    : codes_(values.size()), validity_(validity.size()), row_count_(values.size()) {
  assert(values.size() == validity.size());
  assert(values.size() <= kVectorSize);
  zone_map_.row_count = static_cast<std::uint32_t>(row_count_);

  // Reserved up front so dictionary_ never reallocates: code_of below holds
  // string_views pointing *into* dictionary_'s elements, and a vector
  // reallocation would move every std::string object (invalidating those
  // views, including for small strings using SSO, where the characters
  // live inside the moved object itself). Reserve for the worst case
  // (every row a distinct value) rather than track cardinality up front.
  dictionary_.reserve(row_count_);
  std::unordered_map<std::string_view, std::uint32_t> code_of;

  for (std::size_t i = 0; i < row_count_; ++i) {
    validity_.set(i, validity[i]);
    if (!validity[i]) {
      codes_[i] = 0;
      zone_map_.observe_null();
      continue;
    }
    const std::string_view v = values[i];
    auto it = code_of.find(v);
    std::uint32_t code;
    if (it == code_of.end()) {
      code = static_cast<std::uint32_t>(dictionary_.size());
      dictionary_.emplace_back(v);
      code_of.emplace(std::string_view(dictionary_.back()), code);
    } else {
      code = it->second;
    }
    codes_[i] = code;
    zone_map_.observe(v);
  }
}

// -------------------------------------------------------- DictionaryRle --

DictionaryRleColumnChunk::DictionaryRleColumnChunk(
    std::span<const std::string_view> values, std::span<const bool> validity)
    : row_count_(values.size()) {
  assert(values.size() == validity.size());
  assert(values.size() <= kVectorSize);
  zone_map_.row_count = static_cast<std::uint32_t>(row_count_);

  dictionary_.reserve(row_count_);
  std::unordered_map<std::string_view, std::uint32_t> code_of;
  auto code_for = [&](std::string_view v) -> std::uint32_t {
    auto it = code_of.find(v);
    if (it != code_of.end()) return it->second;
    const auto code = static_cast<std::uint32_t>(dictionary_.size());
    dictionary_.emplace_back(v);
    code_of.emplace(std::string_view(dictionary_.back()), code);
    return code;
  };

  std::size_t i = 0;
  while (i < row_count_) {
    const bool null_run = !validity[i];
    std::uint32_t code = 0;
    if (null_run) {
      zone_map_.observe_null();
    } else {
      code = code_for(values[i]);
      zone_map_.observe(values[i]);
    }

    std::size_t j = i + 1;
    while (j < row_count_) {
      const bool j_null = !validity[j];
      if (j_null != null_run) break;
      if (!null_run && values[j] != values[i]) break;
      if (null_run) {
        zone_map_.observe_null();
      } else {
        zone_map_.observe(values[j]);
      }
      ++j;
    }

    runs_.push_back(RleRun{code, static_cast<std::uint32_t>(j - i), null_run});
    i = j;
  }
}

bool DictionaryRleColumnChunk::is_null(std::size_t i) const noexcept {
  std::size_t cursor = 0;
  for (const RleRun& run : runs_) {
    if (i < cursor + run.length) {
      return run.is_null;
    }
    cursor += run.length;
  }
  return false;  // unreachable when i < row_count_
}

std::string_view DictionaryRleColumnChunk::value(std::size_t i) const {
  std::size_t cursor = 0;
  for (const RleRun& run : runs_) {
    if (i < cursor + run.length) {
      if (run.is_null) {
        throw std::logic_error(
            "DictionaryRleColumnChunk::value called on a null row");
      }
      return dictionary_[run.code];
    }
    cursor += run.length;
  }
  throw std::out_of_range("DictionaryRleColumnChunk::value index out of range");
}

std::vector<std::optional<std::uint32_t>> DictionaryRleColumnChunk::decode_to_codes()
    const {
  throw std::logic_error(
      "DictionaryRleColumnChunk::decode_to_codes is a Phase 1 stub -- "
      "implement it (see the spec comment in column_chunk.hpp).");
}

// -------------------------------------------------------------- Variant --

std::size_t row_count(const ColumnChunkVariant& chunk) {
  return std::visit([](const auto& c) { return c.row_count(); }, chunk);
}

std::uint32_t null_count(const ColumnChunkVariant& chunk) {
  return std::visit([](const auto& c) -> std::uint32_t { return c.zone_map().null_count; },
                     chunk);
}

TypeId type_id(const ColumnChunkVariant& chunk) {
  return std::visit(
      [](const auto& c) -> TypeId {
        using ChunkT = std::decay_t<decltype(c)>;
        return ChunkT::kTypeId;
      },
      chunk);
}

Encoding encoding(const ColumnChunkVariant& chunk) {
  return std::visit(
      [](const auto& c) -> Encoding {
        using ChunkT = std::decay_t<decltype(c)>;
        return ChunkT::kEncoding;
      },
      chunk);
}

}  // namespace columnar
