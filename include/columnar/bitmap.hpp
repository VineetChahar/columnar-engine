#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "columnar/aligned_buffer.hpp"

namespace columnar {

// A packed bit array, 1 bit per row. Used both as a column's validity bitmap
// (bit set == value present, per Arrow convention -- see DESIGN.md section 3)
// and, for BoolColumnChunk, as the data storage itself.
//
// Kept separate from the typed data array on purpose: an all-valid chunk
// (the common case) can skip validity checks entirely once the zone map
// reports null_count == 0, without ever touching this class.
class Bitmap {
 public:
  Bitmap() = default;

  // All bits initialized to `default_value` (true == valid/set).
  explicit Bitmap(std::size_t num_bits, bool default_value = true)
      : num_bits_(num_bits),
        words_(word_count(num_bits)) {
    const std::uint8_t fill = default_value ? 0xFFU : 0x00U;
    for (std::size_t i = 0; i < words_.size(); ++i) {
      words_[i] = fill;
    }
    clear_tail_padding();
  }

  std::size_t size() const noexcept { return num_bits_; }

  bool test(std::size_t i) const noexcept {
    return (words_[i / 8] >> (i % 8)) & 0x1U;
  }

  void set(std::size_t i, bool value) noexcept {
    std::uint8_t& word = words_[i / 8];
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << (i % 8));
    word = value ? static_cast<std::uint8_t>(word | mask)
                 : static_cast<std::uint8_t>(word & ~mask);
  }

  // Number of set bits in [0, num_bits_). Uses std::popcount per byte --
  // branchless and auto-vectorizable, unlike a bit-by-bit loop.
  std::size_t count_set() const noexcept {
    std::size_t total = 0;
    for (std::uint8_t word : words_) {
      total += static_cast<std::size_t>(std::popcount(word));
    }
    return total;
  }

  std::size_t count_null() const noexcept { return num_bits_ - count_set(); }

  // STUB -- implement me (see LEARNING.md / quiz for Phase 1).
  //
  // Count set bits in the half-open range [start, start + len), without
  // materializing a sub-bitmap copy. Needed in Phase 3 when a selection
  // vector has trimmed a batch to a sub-range and an operator needs "how
  // many surviving rows are non-null" without a scalar per-bit loop.
  //
  // Spec:
  //  - Precondition: start + len <= size(); UB (or an assert) otherwise.
  //  - Equivalent to (but should not be implemented as) building a new
  //    Bitmap of the sub-range and calling count_set() on it.
  //  - Handle the case where [start, start+len) doesn't fall on byte
  //    boundaries: mask the partial first/last byte, popcount the full
  //    bytes in between directly.
  std::size_t count_set_range(std::size_t start, std::size_t len) const {
    (void)start;
    (void)len;
    throw std::logic_error(
        "Bitmap::count_set_range is a Phase 1 stub -- implement it "
        "(see the spec comment above this function).");
  }

  const std::uint8_t* data() const noexcept { return words_.data(); }
  std::uint8_t* data() noexcept { return words_.data(); }
  std::size_t byte_size() const noexcept { return words_.size(); }

 private:
  static std::size_t word_count(std::size_t num_bits) {
    return (num_bits + 7) / 8;
  }

  // Bits beyond num_bits_ in the final byte must stay zero so count_set()
  // (used verbatim, without masking, by callers like zone-map null counts)
  // isn't polluted by padding bits left over from a `default_value == true`
  // fill.
  void clear_tail_padding() noexcept {
    const std::size_t used_bits_in_last_byte = num_bits_ % 8;
    if (used_bits_in_last_byte == 0 || words_.empty()) {
      return;
    }
    const std::uint8_t keep_mask =
        static_cast<std::uint8_t>((1U << used_bits_in_last_byte) - 1U);
    words_[words_.size() - 1] &= keep_mask;
  }

  std::size_t num_bits_ = 0;
  AlignedBuffer<std::uint8_t> words_;
};

}  // namespace columnar
