#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace lob {

// Lock-free single-producer / single-consumer ring buffer.
//
// - Capacity is a power of two, so wrap-around is a mask, not a modulo.
// - Indices increase monotonically; full is `write - read == N`.
// - The producer and consumer each keep a cached copy of the other side's index
//   on their own cache line, so the shared atomic is only re-read when the cached
//   value says the ring looks full (producer) or empty (consumer). This removes
//   most cross-core cache-line traffic.
// - Publication uses release stores and acquire loads; no locks, no CAS.
template <class T, std::size_t N>
class SpscRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
  static constexpr std::size_t kLine = 64;

 public:
  SpscRing() : buf_(std::make_unique<T[]>(N)) {}
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only.
  bool try_push(const T& v) {
    const std::size_t w = prod_.write.load(std::memory_order_relaxed);
    if (w - prod_.read_cache == N) {
      prod_.read_cache = cons_.read.load(std::memory_order_acquire);
      if (w - prod_.read_cache == N) return false;
    }
    buf_[w & (N - 1)] = v;
    prod_.write.store(w + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) {
    const std::size_t r = cons_.read.load(std::memory_order_relaxed);
    if (r == cons_.write_cache) {
      cons_.write_cache = prod_.write.load(std::memory_order_acquire);
      if (r == cons_.write_cache) return false;
    }
    out = buf_[r & (N - 1)];
    cons_.read.store(r + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() { return N; }

 private:
  struct alignas(kLine) ProducerSide {
    std::atomic<std::size_t> write{0};
    std::size_t read_cache{0};
  };
  struct alignas(kLine) ConsumerSide {
    std::atomic<std::size_t> read{0};
    std::size_t write_cache{0};
  };

  ProducerSide prod_;
  ConsumerSide cons_;
  std::unique_ptr<T[]> buf_;
};

}  // namespace lob
