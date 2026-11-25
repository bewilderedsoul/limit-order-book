#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

using OrderId = std::uint64_t;  // assigned by the engine, starts at 1
using Price = std::int64_t;     // integer ticks: never use floating point for prices
using Qty = std::uint32_t;

constexpr OrderId kInvalidOrderId = 0;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
  Limit,     // match what crosses, rest the remainder
  Market,    // match at any price, cancel the remainder
  IOC,       // immediate-or-cancel: match up to the limit, cancel the remainder
  FOK,       // fill-or-kill: fill completely up to the limit or reject with no trades
  PostOnly,  // reject if it would trade on arrival, otherwise rest
};

enum class RejectReason : std::uint8_t {
  None,
  BadQty,
  BadPrice,
  WouldCross,          // PostOnly order that would have taken liquidity
  NotEnoughLiquidity,  // FOK order that could not be filled in full
};

struct Trade {
  OrderId maker;  // resting order
  OrderId taker;  // incoming order
  Price price;    // always the maker's price
  Qty qty;
  Side aggressor;  // side of the taker
};

// Default listener: every callback compiles away to nothing.
struct NullListener {
  void on_accept(OrderId, Side, Price, Qty, OrderType) {}
  void on_reject(OrderId, RejectReason) {}
  void on_trade(const Trade&) {}
  void on_cancel(OrderId, Qty) {}
  void on_modify(OrderId, Price, Qty) {}
};

constexpr std::string_view to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

constexpr std::string_view to_string(OrderType t) {
  switch (t) {
    case OrderType::Limit: return "LIMIT";
    case OrderType::Market: return "MARKET";
    case OrderType::IOC: return "IOC";
    case OrderType::FOK: return "FOK";
    case OrderType::PostOnly: return "POST_ONLY";
  }
  return "?";
}

constexpr std::string_view to_string(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "NONE";
    case RejectReason::BadQty: return "BAD_QTY";
    case RejectReason::BadPrice: return "BAD_PRICE";
    case RejectReason::WouldCross: return "WOULD_CROSS";
    case RejectReason::NotEnoughLiquidity: return "NOT_ENOUGH_LIQUIDITY";
  }
  return "?";
}

}  // namespace lob
