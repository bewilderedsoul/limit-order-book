#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob {

// Two-level bitmap over price indices [0, size).
//
// Bit i of `words_` is set when price level i is non-empty. Bit w of `summary_`
// is set when words_[w] != 0. Finding the next non-empty level above or below a
// price is a couple of count-zero instructions plus, in the worst case, a scan
// of the summary (size / 4096 words), instead of walking every empty tick.
class PriceBitmap {
 public:
  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

  explicit PriceBitmap(std::size_t size)
      : size_(size), words_((size + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

  std::size_t size() const { return size_; }

  bool test(std::size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1u; }

  void set(std::size_t i) {
    const std::size_t w = i >> 6;
    if (words_[w] == 0) summary_[w >> 6] |= bit(w & 63);
    words_[w] |= bit(i & 63);
  }

  void clear(std::size_t i) {
    const std::size_t w = i >> 6;
    words_[w] &= ~bit(i & 63);
    if (words_[w] == 0) summary_[w >> 6] &= ~bit(w & 63);
  }

  // Smallest set index >= i, or npos.
  std::size_t next(std::size_t i) const {
    if (i >= size_) return npos;
    const std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~std::uint64_t{0} << (i & 63));
    if (m) return (w << 6) + std::countr_zero(m);
    const std::size_t nw = next_word(w + 1);
    return nw == npos ? npos : (nw << 6) + std::countr_zero(words_[nw]);
  }

  // Largest set index <= i, or npos. i past the end is clamped.
  std::size_t prev(std::size_t i) const {
    if (size_ == 0) return npos;
    if (i >= size_) i = size_ - 1;
    const std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~std::uint64_t{0} >> (63 - (i & 63)));
    if (m) return (w << 6) + 63 - std::countl_zero(m);
    if (w == 0) return npos;
    const std::size_t pw = prev_word(w - 1);
    return pw == npos ? npos : (pw << 6) + 63 - std::countl_zero(words_[pw]);
  }

 private:
  static constexpr std::uint64_t bit(std::size_t b) { return std::uint64_t{1} << b; }

  std::size_t next_word(std::size_t w) const {
    if (w >= words_.size()) return npos;
    std::size_t s = w >> 6;
    std::uint64_t m = summary_[s] & (~std::uint64_t{0} << (w & 63));
    while (!m) {
      if (++s >= summary_.size()) return npos;
      m = summary_[s];
    }
    return (s << 6) + std::countr_zero(m);
  }

  std::size_t prev_word(std::size_t w) const {
    std::size_t s = w >> 6;
    std::uint64_t m = summary_[s] & (~std::uint64_t{0} >> (63 - (w & 63)));
    while (!m) {
      if (s == 0) return npos;
      m = summary_[--s];
    }
    return (s << 6) + 63 - std::countl_zero(m);
  }

  std::size_t size_;
  std::vector<std::uint64_t> words_;
  std::vector<std::uint64_t> summary_;
};

}  // namespace lob
