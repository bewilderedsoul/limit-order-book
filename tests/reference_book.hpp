#pragma once

// Deliberately naive order book: std::map of std::deque, linear-scan cancel.
// It is written for obviousness, not speed, and serves two purposes:
//   1. oracle for the differential fuzz test (same inputs => same events)
//   2. baseline for the benchmark
// It mirrors the semantics documented in lob/order_book.hpp exactly.

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob::test {

template <class Listener = NullListener>
class ReferenceBook {
 public:
  ReferenceBook(const BookConfig& cfg, Listener& l) : cfg_(cfg), listener_(l) {}

  OrderId submit(Side side, Price price, Qty qty, OrderType type = OrderType::Limit) {
    const OrderId id = next_id_++;
    if (qty == 0) { listener_.on_reject(id, RejectReason::BadQty); return id; }
    if (type == OrderType::Market) {
      price = side == Side::Buy ? cfg_.max_price : cfg_.min_price;
    } else if (price < cfg_.min_price || price > cfg_.max_price) {
      listener_.on_reject(id, RejectReason::BadPrice);
      return id;
    }
    if (type == OrderType::PostOnly && crosses(side, price)) {
      listener_.on_reject(id, RejectReason::WouldCross);
      return id;
    }
    if (type == OrderType::FOK && available(side, price) < qty) {
      listener_.on_reject(id, RejectReason::NotEnoughLiquidity);
      return id;
    }
    listener_.on_accept(id, side, price, qty, type);
    const Qty left = match(id, side, price, qty);
    if (left == 0) return id;
    if (type == OrderType::Limit || type == OrderType::PostOnly) rest(id, side, price, left);
    else listener_.on_cancel(id, left);
    return id;
  }

  bool cancel(OrderId id) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    const Qty q = erase(id, it->second.first, it->second.second);
    listener_.on_cancel(id, q);
    return true;
  }

  bool modify(OrderId id, Price new_price, Qty new_qty) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    if (new_qty == 0) return cancel(id);
    if (new_price < cfg_.min_price || new_price > cfg_.max_price) return false;
    const auto [side, price] = it->second;
    if (new_price == price) {
      auto& q = queue(side, price);
      auto o = std::find_if(q.begin(), q.end(), [&](const O& x) { return x.id == id; });
      if (new_qty <= o->qty) {
        o->qty = new_qty;
        listener_.on_modify(id, new_price, new_qty);
        return true;
      }
    }
    erase(id, side, price);
    listener_.on_modify(id, new_price, new_qty);
    const Qty left = match(id, side, new_price, new_qty);
    if (left > 0) rest(id, side, new_price, left);
    return true;
  }

  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
  }
  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }

  std::vector<LevelView> depth(Side side) const {
    std::vector<LevelView> out;
    auto add = [&](const auto& book) {
      for (const auto& [px, q] : book) {
        std::uint64_t t = 0;
        for (const O& o : q) t += o.qty;
        out.push_back({px, t, static_cast<std::uint32_t>(q.size())});
      }
    };
    if (side == Side::Buy) add(bids_); else add(asks_);
    return out;
  }

 private:
  struct O { OrderId id; Qty qty; };

  std::deque<O>& queue(Side s, Price p) { return s == Side::Buy ? bids_[p] : asks_[p]; }

  bool crosses(Side side, Price price) const {
    if (side == Side::Buy) return !asks_.empty() && asks_.begin()->first <= price;
    return !bids_.empty() && bids_.begin()->first >= price;
  }

  std::uint64_t available(Side side, Price price) const {
    std::uint64_t sum = 0;
    if (side == Side::Buy) {
      for (const auto& [px, q] : asks_) { if (px > price) break; for (const O& o : q) sum += o.qty; }
    } else {
      for (const auto& [px, q] : bids_) { if (px < price) break; for (const O& o : q) sum += o.qty; }
    }
    return sum;
  }

  template <class Book, class Crosses>
  Qty match_book(Book& book, OrderId taker, Side aggr, Qty qty, Crosses ok) {
    while (qty > 0 && !book.empty() && ok(book.begin()->first)) {
      auto& [px, q] = *book.begin();
      while (qty > 0 && !q.empty()) {
        O& m = q.front();
        const Qty f = std::min(qty, m.qty);
        m.qty -= f;
        qty -= f;
        listener_.on_trade(Trade{m.id, taker, px, f, aggr});
        if (m.qty == 0) { where_.erase(m.id); q.pop_front(); }
      }
      if (q.empty()) book.erase(book.begin());
    }
    return qty;
  }

  Qty match(OrderId taker, Side side, Price limit, Qty qty) {
    if (side == Side::Buy) return match_book(asks_, taker, side, qty, [&](Price p) { return p <= limit; });
    return match_book(bids_, taker, side, qty, [&](Price p) { return p >= limit; });
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    queue(side, price).push_back({id, qty});
    where_[id] = {side, price};
  }

  Qty erase(OrderId id, Side side, Price price) {
    auto& q = queue(side, price);
    auto o = std::find_if(q.begin(), q.end(), [&](const O& x) { return x.id == id; });
    const Qty qty = o->qty;
    q.erase(o);
    if (q.empty()) { if (side == Side::Buy) bids_.erase(price); else asks_.erase(price); }
    where_.erase(id);
    return qty;
  }

  BookConfig cfg_;
  Listener& listener_;
  std::map<Price, std::deque<O>, std::greater<>> bids_;
  std::map<Price, std::deque<O>> asks_;
  std::unordered_map<OrderId, std::pair<Side, Price>> where_;
  OrderId next_id_ = 1;
};

}  // namespace lob::test
