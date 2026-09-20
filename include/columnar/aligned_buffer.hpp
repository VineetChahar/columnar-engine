#pragma once

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>

namespace columnar {

// The one place in this codebase that touches raw allocation. Every other
// type builds on top of this (or on std::vector/std::string, which manage
// their own memory). AlignedBuffer<T> owns a heap array of N trivially
// constructible T's aligned to `Alignment` bytes, released automatically
// on destruction (or on move) -- RAII in place of manual new[]/delete[].
//
// Alignment matters here because chunk data is later mmap'd straight off
// disk: a data block that starts at a cache-line-aligned (64B) offset can be
// reinterpret_cast to a typed pointer without unaligned-access penalties, and
// keeping in-memory buffers aligned the same way makes an in-memory chunk and
// an mmap'd chunk behave identically to every consumer. See DESIGN.md
// section 6.
template <typename T, std::size_t Alignment = 64>
class AlignedBuffer {
 public:
  static_assert(std::is_trivially_copyable_v<T>,
                "AlignedBuffer is for POD-like column data, not owning types");
  static_assert(Alignment >= alignof(T),
                "Alignment must be at least as strict as T's natural alignment");

  AlignedBuffer() = default;

  explicit AlignedBuffer(std::size_t count) : size_(count) {
    if (count == 0) {
      return;
    }
    const std::size_t bytes = round_up(count * sizeof(T), Alignment);
    data_ = static_cast<T*>(
        ::operator new(bytes, std::align_val_t{Alignment}));
  }

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  AlignedBuffer(AlignedBuffer&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)) {}

  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
      release();
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  ~AlignedBuffer() { release(); }

  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  T& operator[](std::size_t i) noexcept { return data_[i]; }
  const T& operator[](std::size_t i) const noexcept { return data_[i]; }

  T* begin() noexcept { return data_; }
  T* end() noexcept { return data_ + size_; }
  const T* begin() const noexcept { return data_; }
  const T* end() const noexcept { return data_ + size_; }

 private:
  static constexpr std::size_t round_up(std::size_t n, std::size_t multiple) {
    return ((n + multiple - 1) / multiple) * multiple;
  }

  void release() noexcept {
    if (data_ != nullptr) {
      ::operator delete(data_, std::align_val_t{Alignment});
      data_ = nullptr;
    }
  }

  T* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace columnar
