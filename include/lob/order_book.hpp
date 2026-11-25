#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "lob/pool.hpp"
#include "lob/price_bitmap.hpp"
#include "lob/types.hpp"

namespace lob {

struct BookConfig {
  Price min_price = 1;           // lowest valid tick
  Price max_price = 100'000;     // highest valid tick
  std::uint32_t max_orders = 1u << 20;  // resting-order capacity
};

struct LevelView {
  Price price;
  std::uint64_t qty;
  std::uint32_t orders;
  bool operator==(const LevelView&) const = default;
};

// Single-instrument limit order book with price-time priority.
//
// Layout
//   - Price levels live in two flat arrays (bids, asks) indexed by tick offset
//     from min_price, so locating a level is one subtraction.
//   - Each level is a FIFO intrusive doubly linked list of orders, linked by
//     32-bit pool slots. Cancel from the middle of the queue is O(1).
//   - A PriceBitmap per side finds the next non-empty level when the best one
//     empties, without scanning every empty tick.
//   - OrderIds are assigned sequentially by the engine, so id -> slot lookup is
//     a vector index rather than a hash map.
//
// Events are delivered to `Listener` (a template parameter, not a virtual
// interface) so the compiler can inline them; NullListener costs nothing.
// Not thread-safe: one book is owned by one thread (see spsc_ring.hpp for how
// orders reach it).
template <class Listener = NullListener>
class OrderBook {
 public:
  OrderBook(const BookConfig& cfg, Listener& listener)
      : cfg_(validate(cfg)),
        n_levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)),
        listener_(listener),
        bids_(n_levels_),
        asks_(n_levels_),
        bid_bits_(n_levels_),
        ask_bits_(n_levels_),
        pool_(cfg.max_orders) {
    id_to_slot_.reserve(static_cast<std::size_t>(cfg.max_orders) + 1);
    id_to_slot_.push_back(kNullSlot);  // id 0 is never valid
  }

  // Submits a new order and returns its id. Every call consumes an id, even if
  // the order is rejected, so ids are predictable by the sender.
  // For Market orders `price` is ignored.
  OrderId submit(Side side, Price price, Qty qty, OrderType type = OrderType::Limit) {
    const OrderId id = next_id_++;
    id_to_slot_.push_back(kNullSlot);

    if (qty == 0) return reject(id, RejectReason::BadQty);
    if (type == OrderType::Market) {
      price = side == Side::Buy ? cfg_.max_price : cfg_.min_price;
    } else if (price < cfg_.min_price || price > cfg_.max_price) {
      return reject(id, RejectReason::BadPrice);
    }
    if (type == OrderType::PostOnly && crosses(side, price)) {
      return reject(id, RejectReason::WouldCross);
    }
    if (type == OrderType::FOK && available(side, price, qty) < qty) {
      return reject(id, RejectReason::NotEnoughLiquidity);
    }

    listener_.on_accept(id, side, price, qty, type);
    const Qty left = match(id, side, price, qty);
    if (left == 0) return id;

    if (type == OrderType::Limit || type == OrderType::PostOnly) {
      rest(id, side, price, left);
    } else {
      listener_.on_cancel(id, left);  // Market / IOC remainder; FOK never gets here
    }
    return id;
  }

  // Cancels a resting order. Returns false if it is unknown, filled or already cancelled.
  bool cancel(OrderId id) {
    const std::uint32_t s = slot_of(id);
    if (s == kNullSlot) return false;
    const Qty q = pool_[s].qty;
    remove_resting(s);
    listener_.on_cancel(id, q);
    return true;
  }

  // Amends a resting order.
  //   - same price, qty reduced: modified in place, keeps time priority
  //   - price changed or qty increased: loses priority (moves to the back of the
  //     queue at the new price) and may trade if the new price crosses
  //   - new_qty == 0: equivalent to cancel
  // Returns false if the order is not resting or the new price is out of range.
  bool modify(OrderId id, Price new_price, Qty new_qty) {
    const std::uint32_t s = slot_of(id);
    if (s == kNullSlot) return false;
    if (new_qty == 0) return cancel(id);
    if (new_price < cfg_.min_price || new_price > cfg_.max_price) return false;

    OrderNode& n = pool_[s];
    if (new_price == n.price && new_qty <= n.qty) {
      level(n.side, index_of(n.price)).total -= (n.qty - new_qty);
      n.qty = new_qty;
      listener_.on_modify(id, new_price, new_qty);
      return true;
    }

    const Side side = n.side;
    remove_resting(s);
    listener_.on_modify(id, new_price, new_qty);
    const Qty left = match(id, side, new_price, new_qty);
    if (left > 0) rest(id, side, new_price, left);
    return true;
  }

  std::optional<Price> best_bid() const {
    return best_bid_ == PriceBitmap::npos ? std::nullopt : std::optional<Price>(price_at(best_bid_));
  }
  std::optional<Price> best_ask() const {
    return best_ask_ == PriceBitmap::npos ? std::nullopt : std::optional<Price>(price_at(best_ask_));
  }

  // Top `max_levels` aggregated levels, best first.
  std::vector<LevelView> depth(Side side, std::size_t max_levels = SIZE_MAX) const {
    std::vector<LevelView> out;
    std::size_t idx = side == Side::Buy ? best_bid_ : best_ask_;
    while (idx != PriceBitmap::npos && out.size() < max_levels) {
      const Level& l = side == Side::Buy ? bids_[idx] : asks_[idx];
      out.push_back({price_at(idx), l.total, l.count});
      if (side == Side::Buy) {
        idx = idx == 0 ? PriceBitmap::npos : bid_bits_.prev(idx - 1);
      } else {
        idx = ask_bits_.next(idx + 1);
      }
    }
    return out;
  }

  // Remaining quantity of a resting order, or nullopt.
  std::optional<Qty> resting_qty(OrderId id) const {
    const std::uint32_t s = slot_of(id);
    return s == kNullSlot ? std::nullopt : std::optional<Qty>(pool_[s].qty);
  }

  std::uint32_t resting_orders() const { return pool_.in_use(); }
  const BookConfig& config() const { return cfg_; }

 private:
  struct OrderNode {
    OrderId id;
    Price price;
    Qty qty;
    std::uint32_t prev;
    std::uint32_t next;
    Side side;
  };
  static_assert(sizeof(OrderNode) <= 32, "two orders per cache line");

  struct Level {
    std::uint32_t head = kNullSlot;
    std::uint32_t tail = kNullSlot;
    std::uint32_t count = 0;
    std::uint64_t total = 0;
  };

  static const BookConfig& validate(const BookConfig& c) {
    if (c.max_price < c.min_price) throw std::invalid_argument("max_price < min_price");
    if (c.max_orders == 0 || c.max_orders == kNullSlot) throw std::invalid_argument("bad max_orders");
    return c;
  }

  OrderId reject(OrderId id, RejectReason r) {
    listener_.on_reject(id, r);
    return id;
  }

  std::size_t index_of(Price p) const { return static_cast<std::size_t>(p - cfg_.min_price); }
  Price price_at(std::size_t idx) const { return cfg_.min_price + static_cast<Price>(idx); }
  Level& level(Side s, std::size_t idx) { return s == Side::Buy ? bids_[idx] : asks_[idx]; }

  std::uint32_t slot_of(OrderId id) const {
    return id < id_to_slot_.size() ? id_to_slot_[id] : kNullSlot;
  }

  bool crosses(Side side, Price price) const {
    if (side == Side::Buy) return best_ask_ != PriceBitmap::npos && price_at(best_ask_) <= price;
    return best_bid_ != PriceBitmap::npos && price_at(best_bid_) >= price;
  }

  // Opposite-side quantity at prices that `price` crosses, stopping once `want` is reached.
  std::uint64_t available(Side side, Price price, Qty want) const {
    std::uint64_t sum = 0;
    if (side == Side::Buy) {
      for (std::size_t i = best_ask_; i != PriceBitmap::npos && price_at(i) <= price && sum < want;
           i = ask_bits_.next(i + 1)) {
        sum += asks_[i].total;
      }
    } else {
      for (std::size_t i = best_bid_; i != PriceBitmap::npos && price_at(i) >= price && sum < want;
           i = i == 0 ? PriceBitmap::npos : bid_bits_.prev(i - 1)) {
        sum += bids_[i].total;
      }
    }
    return sum;
  }

  Qty match(OrderId taker, Side side, Price limit, Qty qty) {
    if (side == Side::Buy) {
      while (qty > 0 && best_ask_ != PriceBitmap::npos && price_at(best_ask_) <= limit) {
        Level& l = asks_[best_ask_];
        qty = consume(l, best_ask_, taker, side, qty);
        if (l.count == 0) {
          ask_bits_.clear(best_ask_);
          best_ask_ = ask_bits_.next(best_ask_);
        }
      }
    } else {
      while (qty > 0 && best_bid_ != PriceBitmap::npos && price_at(best_bid_) >= limit) {
        Level& l = bids_[best_bid_];
        qty = consume(l, best_bid_, taker, side, qty);
        if (l.count == 0) {
          bid_bits_.clear(best_bid_);
          best_bid_ = bid_bits_.prev(best_bid_);
        }
      }
    }
    return qty;
  }

  // Fills against one level in FIFO order. Leaves the bitmap/best update to the caller.
  Qty consume(Level& l, std::size_t idx, OrderId taker, Side aggressor, Qty qty) {
    const Price px = price_at(idx);
    while (qty > 0 && l.head != kNullSlot) {
      const std::uint32_t s = l.head;
      OrderNode& m = pool_[s];
      const Qty fill = std::min(qty, m.qty);
      m.qty -= fill;
      qty -= fill;
      l.total -= fill;
      listener_.on_trade(Trade{m.id, taker, px, fill, aggressor});
      if (m.qty == 0) {
        id_to_slot_[m.id] = kNullSlot;
        unlink(l, s);
        pool_.release(s);
      }
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t s = pool_.alloc();
    if (s == kNullSlot) {  // book full: report the remainder as cancelled
      listener_.on_cancel(id, qty);
      return;
    }
    pool_[s] = OrderNode{id, price, qty, kNullSlot, kNullSlot, side};
    id_to_slot_[id] = s;

    const std::size_t idx = index_of(price);
    if (side == Side::Buy) {
      append(bids_[idx], s);
      bid_bits_.set(idx);
      if (best_bid_ == PriceBitmap::npos || idx > best_bid_) best_bid_ = idx;
    } else {
      append(asks_[idx], s);
      ask_bits_.set(idx);
      if (best_ask_ == PriceBitmap::npos || idx < best_ask_) best_ask_ = idx;
    }
  }

  void remove_resting(std::uint32_t s) {
    OrderNode& n = pool_[s];
    const std::size_t idx = index_of(n.price);
    if (n.side == Side::Buy) {
      Level& l = bids_[idx];
      l.total -= n.qty;
      unlink(l, s);
      if (l.count == 0) {
        bid_bits_.clear(idx);
        if (best_bid_ == idx) best_bid_ = bid_bits_.prev(idx);
      }
    } else {
      Level& l = asks_[idx];
      l.total -= n.qty;
      unlink(l, s);
      if (l.count == 0) {
        ask_bits_.clear(idx);
        if (best_ask_ == idx) best_ask_ = ask_bits_.next(idx);
      }
    }
    id_to_slot_[n.id] = kNullSlot;
    pool_.release(s);
  }

  void append(Level& l, std::uint32_t s) {
    OrderNode& n = pool_[s];
    n.prev = l.tail;
    n.next = kNullSlot;
    if (l.tail != kNullSlot) {
      pool_[l.tail].next = s;
    } else {
      l.head = s;
    }
    l.tail = s;
    ++l.count;
    l.total += n.qty;
  }

  // Removes slot s from its level's list. Does not touch l.total.
  void unlink(Level& l, std::uint32_t s) {
    OrderNode& n = pool_[s];
    if (n.prev != kNullSlot) pool_[n.prev].next = n.next; else l.head = n.next;
    if (n.next != kNullSlot) pool_[n.next].prev = n.prev; else l.tail = n.prev;
    --l.count;
  }

  BookConfig cfg_;
  std::size_t n_levels_;
  Listener& listener_;

  std::vector<Level> bids_;
  std::vector<Level> asks_;
  PriceBitmap bid_bits_;
  PriceBitmap ask_bits_;
  std::size_t best_bid_ = PriceBitmap::npos;
  std::size_t best_ask_ = PriceBitmap::npos;

  Pool<OrderNode> pool_;
  std::vector<std::uint32_t> id_to_slot_;
  OrderId next_id_ = 1;
};

}  // namespace lob
