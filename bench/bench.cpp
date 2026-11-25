// Benchmarks:
//   1. throughput of OrderBook vs the naive std::map ReferenceBook on identical flow
//   2. per-operation latency percentiles (submit / cancel / modify)
//   3. two-thread pipeline: gateway thread -> SpscRing -> matching thread
//
// Usage: lob_bench [num_ops]   (default 2,000,000)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../tests/reference_book.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;
using Clock = std::chrono::steady_clock;

namespace {

struct CountingListener {
  std::uint64_t trades = 0, volume = 0, accepts = 0, cancels = 0;
  void on_accept(OrderId, Side, Price, Qty, OrderType) { ++accepts; }
  void on_reject(OrderId, RejectReason) {}
  void on_trade(const Trade& t) { ++trades; volume += t.qty; }
  void on_cancel(OrderId, Qty) { ++cancels; }
  void on_modify(OrderId, Price, Qty) {}
};

constexpr Price kMin = 1, kMax = 200'000, kMid = 100'000;

BookConfig config(std::size_t n) {
  return BookConfig{kMin, kMax, static_cast<std::uint32_t>(std::max<std::size_t>(n, 1024))};
}

template <class Book>
double run_throughput(Book& book, const std::vector<Command>& cmds) {
  const auto t0 = Clock::now();
  for (const auto& c : cmds) apply(book, c);
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  return static_cast<double>(cmds.size()) / secs;
}

double timer_overhead_ns() {
  constexpr int k = 100'000;
  std::int64_t total = 0;
  for (int i = 0; i < k; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    total += std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
  }
  return static_cast<double>(total) / k;
}

void print_percentiles(const char* name, std::vector<std::uint32_t>& v, double overhead) {
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) {
    const double raw = v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))];
    return std::max(0.0, raw - overhead);
  };
  std::printf("  %-8s %10zu %8.0f %8.0f %8.0f %8.0f %10.0f\n", name, v.size(), pct(0.50), pct(0.90), pct(0.99),
              pct(0.999), std::max(0.0, static_cast<double>(v.back()) - overhead));
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  std::printf("Generating %zu commands...\n", n);
  const auto cmds = make_workload(n, 42, kMid, kMin, kMax);

  // ---- 1. throughput
  CountingListener fl;
  double fast_ops;
  std::uint32_t resting_end;
  {
    OrderBook book(config(n), fl);
    fast_ops = run_throughput(book, cmds);
    resting_end = book.resting_orders();
  }
  CountingListener rl;
  double ref_ops;
  {
    test::ReferenceBook ref(config(n), rl);
    ref_ops = run_throughput(ref, cmds);
  }
  if (fl.trades != rl.trades || fl.volume != rl.volume) {
    std::fprintf(stderr, "MISMATCH between OrderBook and ReferenceBook\n");
    return 1;
  }
  std::printf("\nThroughput (single thread, %zu ops, %llu trades, %u orders resting at end)\n", n,
              static_cast<unsigned long long>(fl.trades), resting_end);
  std::printf("  OrderBook       %8.2f M ops/s  (%6.1f ns/op)\n", fast_ops / 1e6, 1e9 / fast_ops);
  std::printf("  std::map book   %8.2f M ops/s  (%6.1f ns/op)\n", ref_ops / 1e6, 1e9 / ref_ops);
  std::printf("  speed-up        %8.1fx\n", fast_ops / ref_ops);

  // ---- 2. latency
  {
    CountingListener l;
    OrderBook book(config(n), l);
    std::vector<std::uint32_t> sub, can, mod;
    sub.reserve(n); can.reserve(n / 2); mod.reserve(n / 10);
    const double overhead = timer_overhead_ns();
    for (const auto& c : cmds) {
      const auto a = Clock::now();
      apply(book, c);
      const auto b = Clock::now();
      const auto ns = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      (c.kind == Command::Kind::Submit ? sub : c.kind == Command::Kind::Cancel ? can : mod).push_back(ns);
    }
    std::printf("\nLatency per operation in ns (timer overhead %.0f ns subtracted)\n", overhead);
    std::printf("  %-8s %10s %8s %8s %8s %8s %10s\n", "op", "count", "p50", "p90", "p99", "p99.9", "max");
    print_percentiles("submit", sub, overhead);
    print_percentiles("cancel", can, overhead);
    print_percentiles("modify", mod, overhead);
  }

  // ---- 3. gateway thread -> SPSC ring -> matching thread
  {
    CountingListener l;
    OrderBook book(config(n), l);
    auto ring = std::make_unique<SpscRing<Command, 4096>>();
    const auto t0 = Clock::now();
    std::thread engine([&] {
      Command c;
      for (;;) {
        if (!ring->try_pop(c)) continue;
        if (c.kind == Command::Kind::Stop) break;
        apply(book, c);
      }
    });
    for (const auto& c : cmds) while (!ring->try_push(c)) {}
    Command stop;
    stop.kind = Command::Kind::Stop;
    while (!ring->try_push(stop)) {}
    engine.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("\nPipeline (gateway thread -> SpscRing<4096> -> matching thread)\n");
    std::printf("  %8.2f M ops/s end to end, %llu trades (expected %llu)\n", static_cast<double>(n) / secs / 1e6,
                static_cast<unsigned long long>(l.trades), static_cast<unsigned long long>(fl.trades));
    if (l.trades != fl.trades) return 1;
  }
  return 0;
}
