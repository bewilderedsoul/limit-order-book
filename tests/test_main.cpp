// Self-contained test runner (no external dependencies).

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/price_bitmap.hpp"
#include "lob/spsc_ring.hpp"
#include "recording_listener.hpp"
#include "reference_book.hpp"

using namespace lob;
using lob::test::Event;
using lob::test::RecordingListener;
using lob::test::ReferenceBook;
using K = Event::Kind;

namespace {

struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Register { Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
int g_failures = 0;

#define TEST(name) \
  static void name(); \
  static Register reg_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::fprintf(stderr, "  CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
    }                                                                            \
  } while (0)

Event trade(OrderId maker, OrderId taker, Price px, Qty q, Side aggr) {
  return Event{K::Trade, maker, taker, px, q, aggr};
}
Event cancel_ev(OrderId id, Qty q) { Event e{K::Cancel, id}; e.qty = q; return e; }
Event reject_ev(OrderId id, RejectReason r) { Event e{K::Reject, id}; e.reason = r; return e; }

std::vector<Event> only(const std::vector<Event>& ev, K kind) {
  std::vector<Event> out;
  for (const auto& e : ev) if (e.kind == kind) out.push_back(e);
  return out;
}

BookConfig small_cfg() { return BookConfig{1, 1000, 1024}; }

}  // namespace

// ---------------------------------------------------------------- basics

TEST(resting_orders_set_top_of_book) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  CHECK(!book.best_bid() && !book.best_ask());
  book.submit(Side::Buy, 99, 10);
  book.submit(Side::Buy, 98, 5);
  book.submit(Side::Sell, 101, 7);
  book.submit(Side::Sell, 101, 3);
  CHECK(book.best_bid() == 99);
  CHECK(book.best_ask() == 101);
  CHECK(book.depth(Side::Buy) == (std::vector<LevelView>{{99, 10, 1}, {98, 5, 1}}));
  CHECK(book.depth(Side::Sell) == (std::vector<LevelView>{{101, 10, 2}}));
  CHECK(only(L.events, K::Trade).empty());
  CHECK(book.resting_orders() == 4);
}

TEST(price_time_priority_within_level) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 10);
  const auto b = book.submit(Side::Sell, 100, 10);
  const auto t = book.submit(Side::Buy, 100, 15);
  const auto trades = only(L.events, K::Trade);
  CHECK(trades.size() == 2);
  CHECK(trades[0] == trade(a, t, 100, 10, Side::Buy));
  CHECK(trades[1] == trade(b, t, 100, 5, Side::Buy));
  CHECK(book.resting_qty(a) == std::nullopt);
  CHECK(book.resting_qty(b) == 5u);
  CHECK(!book.best_bid());  // taker fully filled, nothing rests
}

TEST(sweep_trades_at_maker_prices_and_rests_remainder) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 5);
  const auto b = book.submit(Side::Sell, 101, 5);
  book.submit(Side::Sell, 110, 5);
  const auto t = book.submit(Side::Buy, 105, 12);
  const auto trades = only(L.events, K::Trade);
  CHECK(trades.size() == 2);
  CHECK(trades[0] == trade(a, t, 100, 5, Side::Buy));
  CHECK(trades[1] == trade(b, t, 101, 5, Side::Buy));
  CHECK(book.best_bid() == 105);
  CHECK(book.resting_qty(t) == 2u);
  CHECK(book.best_ask() == 110);
}

TEST(sell_side_aggressor) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Buy, 100, 5);
  const auto b = book.submit(Side::Buy, 99, 5);
  const auto t = book.submit(Side::Sell, 99, 8);
  const auto trades = only(L.events, K::Trade);
  CHECK(trades.size() == 2);
  CHECK(trades[0] == trade(a, t, 100, 5, Side::Sell));
  CHECK(trades[1] == trade(b, t, 99, 3, Side::Sell));
  CHECK(book.best_bid() == 99);
  CHECK(book.resting_qty(b) == 2u);
}

// ---------------------------------------------------------------- order types

TEST(market_order_fills_then_cancels_remainder) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 500, 4);
  const auto m = book.submit(Side::Buy, 0, 10, OrderType::Market);
  CHECK(only(L.events, K::Trade) == std::vector<Event>{trade(a, m, 500, 4, Side::Buy)});
  CHECK(L.events.back() == cancel_ev(m, 6));
  CHECK(!book.best_bid() && !book.best_ask());
}

TEST(market_order_into_empty_book) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto m = book.submit(Side::Sell, 0, 10, OrderType::Market);
  CHECK(L.events.size() == 2);
  CHECK(L.events[0].kind == K::Accept);
  CHECK(L.events[1] == cancel_ev(m, 10));
}

TEST(ioc_never_rests) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 3);
  book.submit(Side::Sell, 102, 3);
  const auto i = book.submit(Side::Buy, 101, 10, OrderType::IOC);
  CHECK(only(L.events, K::Trade) == std::vector<Event>{trade(a, i, 100, 3, Side::Buy)});
  CHECK(L.events.back() == cancel_ev(i, 7));
  CHECK(!book.best_bid());
  CHECK(book.best_ask() == 102);
}

TEST(fok_all_or_nothing) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  book.submit(Side::Sell, 100, 3);
  book.submit(Side::Sell, 101, 3);
  book.submit(Side::Sell, 103, 100);
  // Only 6 available at <= 101: reject, and the book must be untouched.
  const auto f1 = book.submit(Side::Buy, 101, 7, OrderType::FOK);
  CHECK(L.events.back() == reject_ev(f1, RejectReason::NotEnoughLiquidity));
  CHECK(only(L.events, K::Trade).empty());
  CHECK(book.depth(Side::Sell).size() == 3);
  // Exactly 6 available: fills completely.
  const auto f2 = book.submit(Side::Buy, 101, 6, OrderType::FOK);
  CHECK(only(L.events, K::Trade).size() == 2);
  CHECK(book.resting_qty(f2) == std::nullopt);
  CHECK(book.best_ask() == 103);
}

TEST(post_only_rejects_when_crossing) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  book.submit(Side::Sell, 100, 5);
  const auto p1 = book.submit(Side::Buy, 100, 5, OrderType::PostOnly);
  CHECK(L.events.back() == reject_ev(p1, RejectReason::WouldCross));
  const auto p2 = book.submit(Side::Buy, 99, 5, OrderType::PostOnly);
  CHECK(book.resting_qty(p2) == 5u);
  CHECK(only(L.events, K::Trade).empty());
}

TEST(rejects_bad_input) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Buy, 100, 0);
  const auto b = book.submit(Side::Buy, 0, 5);
  const auto c = book.submit(Side::Sell, 1001, 5);
  CHECK(L.events[0] == reject_ev(a, RejectReason::BadQty));
  CHECK(L.events[1] == reject_ev(b, RejectReason::BadPrice));
  CHECK(L.events[2] == reject_ev(c, RejectReason::BadPrice));
  CHECK(book.resting_orders() == 0);
  CHECK(a == 1 && b == 2 && c == 3);  // rejected orders still consume ids
}

// ---------------------------------------------------------------- cancel / modify

TEST(cancel_semantics) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Buy, 100, 5);
  const auto b = book.submit(Side::Buy, 100, 7);
  CHECK(book.cancel(a));
  CHECK(L.events.back() == cancel_ev(a, 5));
  CHECK(!book.cancel(a));        // double cancel
  CHECK(!book.cancel(9999));     // unknown
  CHECK(!book.cancel(0));        // invalid
  CHECK(book.depth(Side::Buy) == (std::vector<LevelView>{{100, 7, 1}}));
  book.submit(Side::Sell, 100, 7);
  CHECK(!book.cancel(b));        // already filled
  CHECK(!book.best_bid());
}

TEST(cancel_middle_of_queue_keeps_fifo) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 1);
  const auto b = book.submit(Side::Sell, 100, 1);
  const auto c = book.submit(Side::Sell, 100, 1);
  CHECK(book.cancel(b));
  const auto t = book.submit(Side::Buy, 100, 2);
  const auto trades = only(L.events, K::Trade);
  CHECK(trades.size() == 2);
  CHECK(trades[0] == trade(a, t, 100, 1, Side::Buy));
  CHECK(trades[1] == trade(c, t, 100, 1, Side::Buy));
}

TEST(modify_down_keeps_priority) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 10);
  const auto b = book.submit(Side::Sell, 100, 10);
  CHECK(book.modify(a, 100, 4));
  CHECK(book.depth(Side::Sell) == (std::vector<LevelView>{{100, 14, 2}}));
  const auto t = book.submit(Side::Buy, 100, 5);
  const auto trades = only(L.events, K::Trade);
  CHECK(trades[0] == trade(a, t, 100, 4, Side::Buy));  // a still first
  CHECK(trades[1] == trade(b, t, 100, 1, Side::Buy));
}

TEST(modify_up_loses_priority) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Sell, 100, 5);
  const auto b = book.submit(Side::Sell, 100, 5);
  CHECK(book.modify(a, 100, 6));
  const auto t = book.submit(Side::Buy, 100, 5);
  CHECK(only(L.events, K::Trade) == std::vector<Event>{trade(b, t, 100, 5, Side::Buy)});
  CHECK(book.resting_qty(a) == 6u);
}

TEST(modify_price_can_cross) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto s = book.submit(Side::Sell, 105, 5);
  const auto b = book.submit(Side::Buy, 100, 8);
  CHECK(book.modify(b, 105, 8));
  CHECK(only(L.events, K::Trade) == std::vector<Event>{trade(s, b, 105, 5, Side::Buy)});
  CHECK(book.best_bid() == 105);
  CHECK(book.resting_qty(b) == 3u);
  CHECK(!book.best_ask());
}

TEST(modify_edge_cases) {
  RecordingListener L;
  OrderBook book(small_cfg(), L);
  const auto a = book.submit(Side::Buy, 100, 5);
  CHECK(!book.modify(a, 5000, 5));  // out of band: rejected, order untouched
  CHECK(book.resting_qty(a) == 5u);
  CHECK(!book.modify(424242, 100, 5));
  CHECK(book.modify(a, 100, 0));    // qty 0 == cancel
  CHECK(L.events.back() == cancel_ev(a, 5));
  CHECK(!book.best_bid());
}

// ---------------------------------------------------------------- capacity / structure

TEST(book_full_cancels_remainder) {
  RecordingListener L;
  OrderBook book(BookConfig{1, 1000, 2}, L);
  book.submit(Side::Buy, 10, 1);
  book.submit(Side::Buy, 11, 1);
  const auto c = book.submit(Side::Buy, 12, 1);
  CHECK(L.events.back() == cancel_ev(c, 1));
  CHECK(book.resting_orders() == 2);
  CHECK(book.best_bid() == 11);
}

TEST(best_price_recovers_across_sparse_levels) {
  RecordingListener L;
  OrderBook book(BookConfig{1, 1'000'000, 64}, L);
  // Far apart so the bitmap must cross word and summary-word boundaries.
  const auto a1 = book.submit(Side::Sell, 7, 1);
  book.submit(Side::Sell, 300'000, 1);
  book.submit(Side::Sell, 999'999, 1);
  const auto b1 = book.submit(Side::Buy, 2, 1);
  book.submit(Side::Buy, 1, 1);
  CHECK(book.cancel(a1));
  CHECK(book.best_ask() == 300'000);
  book.submit(Side::Buy, 300'000, 1);
  CHECK(book.best_ask() == 999'999);
  CHECK(book.cancel(b1));
  CHECK(book.best_bid() == 1);
  book.submit(Side::Sell, 0, 5, OrderType::Market);
  CHECK(!book.best_bid());
  CHECK(book.best_ask() == 999'999);
}

TEST(bitmap_matches_std_set) {
  std::mt19937_64 rng(7);
  const std::size_t n = 70'000;  // > 4096 so the summary has several words
  PriceBitmap bm(n);
  std::set<std::size_t> ref;
  for (int step = 0; step < 200'000; ++step) {
    const std::size_t i = rng() % n;
    if (rng() & 1) { bm.set(i); ref.insert(i); } else { bm.clear(i); ref.erase(i); }
    const std::size_t q = rng() % n;
    auto it = ref.lower_bound(q);
    const std::size_t want_next = it == ref.end() ? PriceBitmap::npos : *it;
    auto jt = ref.upper_bound(q);
    const std::size_t want_prev = jt == ref.begin() ? PriceBitmap::npos : *std::prev(jt);
    if (bm.next(q) != want_next || bm.prev(q) != want_prev || bm.test(i) != ref.count(i)) {
      CHECK(false);
      return;
    }
  }
  CHECK(bm.next(n) == PriceBitmap::npos);
}

TEST(spsc_ring_preserves_order_across_threads) {
  constexpr std::uint64_t kN = 2'000'000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN; ++i) while (!ring->try_push(i)) {}
  });
  std::uint64_t expected = 0, v = 0;
  bool ok = true;
  while (expected < kN) {
    if (ring->try_pop(v)) {
      if (v != expected) { ok = false; break; }
      ++expected;
    }
  }
  producer.join();
  CHECK(ok);
  CHECK(!ring->try_pop(v));
}

// ---------------------------------------------------------------- differential fuzz

// Drives the fast book and the naive reference book with the same random order
// flow (all order types, cancels, modifies, invalid input) in a narrow price
// band so that crossing is frequent, and requires identical event streams and
// identical full-depth snapshots throughout.
static bool fuzz_once(std::uint64_t seed, int ops) {
  const BookConfig cfg{1, 120, 1u << 16};
  RecordingListener fast_l, ref_l;
  OrderBook fast(cfg, fast_l);
  ReferenceBook ref(cfg, ref_l);
  std::mt19937_64 rng(seed);
  std::vector<OrderId> ids;

  auto rnd = [&](std::uint64_t lo, std::uint64_t hi) { return lo + rng() % (hi - lo + 1); };

  for (int op = 0; op < ops; ++op) {
    const auto r = rnd(0, 99);
    if (r < 50 || ids.empty()) {
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price center = side == Side::Buy ? 58 : 62;
      Price price = static_cast<Price>(center + static_cast<Price>(rnd(0, 16)) - 8);
      Qty qty = static_cast<Qty>(rnd(1, 50));
      const auto t = rnd(0, 99);
      OrderType type = t < 70 ? OrderType::Limit
                     : t < 78 ? OrderType::IOC
                     : t < 86 ? OrderType::FOK
                     : t < 94 ? OrderType::PostOnly
                              : OrderType::Market;
      if (rnd(0, 199) == 0) qty = 0;
      if (rnd(0, 199) == 0) price = rnd(0, 1) ? 0 : 121;
      const auto a = fast.submit(side, price, qty, type);
      const auto b = ref.submit(side, price, qty, type);
      if (a != b) return false;
      ids.push_back(a);
    } else if (r < 80) {
      const OrderId id = ids[rng() % ids.size()];
      if (fast.cancel(id) != ref.cancel(id)) return false;
    } else {
      const OrderId id = ids[rng() % ids.size()];
      const Price p = static_cast<Price>(rnd(48, 72));
      const Qty q = static_cast<Qty>(rnd(0, 60));
      if (fast.modify(id, p, q) != ref.modify(id, p, q)) return false;
    }

    if (fast.best_bid() != ref.best_bid() || fast.best_ask() != ref.best_ask()) return false;
    if (fast_l.events.size() != ref_l.events.size()) return false;
    if (op % 64 == 0) {
      if (fast.depth(Side::Buy) != ref.depth(Side::Buy)) return false;
      if (fast.depth(Side::Sell) != ref.depth(Side::Sell)) return false;
    }
  }
  if (fast_l.events != ref_l.events) {
    for (std::size_t i = 0; i < fast_l.events.size(); ++i) {
      if (!(fast_l.events[i] == ref_l.events[i])) {
        std::fprintf(stderr, "  seed %llu event %zu: fast=%s ref=%s\n", (unsigned long long)seed, i,
                     fast_l.events[i].str().c_str(), ref_l.events[i].str().c_str());
        break;
      }
    }
    return false;
  }
  return fast.depth(Side::Buy) == ref.depth(Side::Buy) && fast.depth(Side::Sell) == ref.depth(Side::Sell);
}

TEST(differential_fuzz_against_reference_book) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    if (!fuzz_once(seed, 20'000)) {
      std::fprintf(stderr, "  divergence with seed %llu\n", (unsigned long long)seed);
      CHECK(false);
      return;
    }
  }
}

int main() {
  int failed_tests = 0;
  for (const auto& t : registry()) {
    const int before = g_failures;
    t.fn();
    const bool ok = g_failures == before;
    if (!ok) ++failed_tests;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
  }
  std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
  return failed_tests == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
