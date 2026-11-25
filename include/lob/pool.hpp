#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace lob {

constexpr std::uint32_t kNullSlot = std::numeric_limits<std::uint32_t>::max();

// Fixed-capacity object pool addressed by 32-bit slot index.
//
// All memory is allocated up front, so adding and removing orders never calls
// the allocator. Slots are recycled LIFO, which keeps recently freed (and
// therefore cache-warm) slots in use. 32-bit indices instead of pointers halve
// the size of the intrusive list links.
template <class T>
class Pool {
 public:
  explicit Pool(std::uint32_t capacity) : slots_(capacity) {
    free_.reserve(capacity);
    for (std::uint32_t i = capacity; i > 0; --i) free_.push_back(i - 1);
  }

  // Returns kNullSlot when the pool is exhausted.
  std::uint32_t alloc() {
    if (free_.empty()) return kNullSlot;
    const std::uint32_t s = free_.back();
    free_.pop_back();
    return s;
  }

  void release(std::uint32_t s) { free_.push_back(s); }

  T& operator[](std::uint32_t s) { return slots_[s]; }
  const T& operator[](std::uint32_t s) const { return slots_[s]; }

  std::uint32_t capacity() const { return static_cast<std::uint32_t>(slots_.size()); }
  std::uint32_t in_use() const { return capacity() - static_cast<std::uint32_t>(free_.size()); }

 private:
  std::vector<T> slots_;
  std::vector<std::uint32_t> free_;
};

}  // namespace lob
