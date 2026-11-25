#pragma once

// Synthetic order flow loosely shaped like a liquid equity book: most orders
// are passive limits a few ticks from a random-walking mid, a large share of
// them are cancelled, and a minority take liquidity.

#include <cstdint>
#include <cmath>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob::bench {

struct Command {
  enum class Kind : std::uint8_t { Submit, Cancel, Modify, Stop };
  Kind kind = Kind::Stop;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;
  Qty qty = 0;
  OrderId id = 0;
};

struct WorkloadMix {
  int submit_pct = 55;
  int cancel_pct = 33;
  int modify_pct = 5;  // remainder: market / IOC takers
};

inline std::vector<Command> make_workload(std::size_t n, std::uint64_t seed, Price mid0, Price lo, Price hi,
                                          WorkloadMix mix = {}) {
  struct Live { OrderId id; Side side; Price price; };
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> offset(0.0, 6.0);
  std::uniform_int_distribution<Qty> qty(1, 100);
  std::vector<Command> out;
  out.reserve(n);
  std::vector<Live> live;
  live.reserve(n / 2);
  OrderId next_id = 1;  // the engine assigns ids sequentially, one per submit
  Price mid = mid0;

  auto clamp = [&](Price p) { return p < lo ? lo : (p > hi ? hi : p); };

  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 63) == 0) mid = clamp(mid + static_cast<Price>(rng() % 3) - 1);
    const int r = static_cast<int>(rng() % 100);
    Command c;
    if (r < mix.submit_pct || live.empty()) {
      c.kind = Command::Kind::Submit;
      c.side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price d = static_cast<Price>(std::abs(offset(rng))) + 1;
      // ~10% of limits are placed through the spread and trade on arrival.
      const bool aggressive = rng() % 10 == 0;
      const Price dist = aggressive ? -static_cast<Price>(rng() % 3) : d;
      c.price = clamp(c.side == Side::Buy ? mid - dist : mid + dist);
      c.qty = qty(rng);
      c.type = OrderType::Limit;
      live.push_back({next_id, c.side, c.price});
      ++next_id;
    } else if (r < mix.submit_pct + mix.cancel_pct) {
      const std::size_t k = rng() % live.size();
      c.kind = Command::Kind::Cancel;
      c.id = live[k].id;
      live[k] = live.back();
      live.pop_back();
    } else if (r < mix.submit_pct + mix.cancel_pct + mix.modify_pct) {
      const Live& l = live[rng() % live.size()];
      c.kind = Command::Kind::Modify;
      c.id = l.id;
      c.price = (rng() & 1) ? l.price : clamp(l.price + static_cast<Price>(rng() % 5) - 2);
      c.qty = qty(rng);
    } else {
      c.kind = Command::Kind::Submit;
      c.side = (rng() & 1) ? Side::Buy : Side::Sell;
      c.type = (rng() & 1) ? OrderType::Market : OrderType::IOC;
      c.price = c.side == Side::Buy ? mid + 2 : mid - 2;
      c.qty = qty(rng);
      ++next_id;
    }
    out.push_back(c);
  }
  return out;
}

template <class Book>
inline void apply(Book& book, const Command& c) {
  switch (c.kind) {
    case Command::Kind::Submit: book.submit(c.side, c.price, c.qty, c.type); break;
    case Command::Kind::Cancel: book.cancel(c.id); break;
    case Command::Kind::Modify: book.modify(c.id, c.price, c.qty); break;
    case Command::Kind::Stop: break;
  }
}

}  // namespace lob::bench
