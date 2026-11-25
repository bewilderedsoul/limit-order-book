#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob::test {

struct Event {
  enum class Kind : std::uint8_t { Accept, Reject, Trade, Cancel, Modify };
  Kind kind{};
  OrderId id = 0;     // order id (maker for trades)
  OrderId other = 0;  // taker for trades
  Price price = 0;
  Qty qty = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  RejectReason reason = RejectReason::None;

  bool operator==(const Event&) const = default;

  std::string str() const {
    std::ostringstream os;
    switch (kind) {
      case Kind::Accept:
        os << "ACCEPT id=" << id << ' ' << to_string(side) << ' ' << to_string(type) << ' ' << qty << '@' << price;
        break;
      case Kind::Reject: os << "REJECT id=" << id << ' ' << to_string(reason); break;
      case Kind::Trade:
        os << "TRADE maker=" << id << " taker=" << other << ' ' << qty << '@' << price << " aggr=" << to_string(side);
        break;
      case Kind::Cancel: os << "CANCEL id=" << id << " qty=" << qty; break;
      case Kind::Modify: os << "MODIFY id=" << id << ' ' << qty << '@' << price; break;
    }
    return os.str();
  }
};

struct RecordingListener {
  std::vector<Event> events;

  void on_accept(OrderId id, Side s, Price p, Qty q, OrderType t) {
    Event e{Event::Kind::Accept, id};
    e.side = s; e.price = p; e.qty = q; e.type = t;
    events.push_back(e);
  }
  void on_reject(OrderId id, RejectReason r) {
    Event e{Event::Kind::Reject, id};
    e.reason = r;
    events.push_back(e);
  }
  void on_trade(const Trade& t) {
    Event e{Event::Kind::Trade, t.maker, t.taker, t.price, t.qty, t.aggressor};
    events.push_back(e);
  }
  void on_cancel(OrderId id, Qty q) {
    Event e{Event::Kind::Cancel, id};
    e.qty = q;
    events.push_back(e);
  }
  void on_modify(OrderId id, Price p, Qty q) {
    Event e{Event::Kind::Modify, id};
    e.price = p; e.qty = q;
    events.push_back(e);
  }
};

}  // namespace lob::test
