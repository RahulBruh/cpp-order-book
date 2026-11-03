#pragma once

#include <ostream>
#include <vector>

#include "lob/types.hpp"

namespace lob::test {

struct Event {
  enum class Kind { Accepted, Rejected, Trade, Cancelled, Modified };
  Kind kind;
  OrderId id = 0;  // taker id for trades
  OrderId maker = 0;
  Price price = 0;
  Qty qty = 0;
  RejectReason reason{};
  friend bool operator==(const Event&, const Event&) = default;
};

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
  static const char* kinds[] = {"Accepted", "Rejected", "Trade", "Cancelled", "Modified"};
  os << kinds[static_cast<int>(e.kind)] << "{id=" << e.id;
  if (e.kind == Event::Kind::Trade) os << " maker=" << e.maker << " px=" << e.price << " qty=" << e.qty;
  if (e.kind == Event::Kind::Cancelled) os << " qty=" << e.qty;
  if (e.kind == Event::Kind::Rejected) os << " reason=" << static_cast<int>(e.reason);
  return os << "}";
}

struct RecordingListener {
  std::vector<Event> events;

  void on_accepted(OrderId id) { events.push_back({Event::Kind::Accepted, id}); }
  void on_rejected(OrderId id, RejectReason r) {
    Event e{Event::Kind::Rejected, id};
    e.reason = r;
    events.push_back(e);
  }
  void on_trade(const Trade& t) { events.push_back({Event::Kind::Trade, t.taker, t.maker, t.price, t.qty}); }
  void on_cancelled(OrderId id, Qty q) { events.push_back({Event::Kind::Cancelled, id, 0, 0, q}); }
  void on_modified(OrderId id) { events.push_back({Event::Kind::Modified, id}); }

  std::vector<Trade> trades() const {
    std::vector<Trade> out;
    for (const auto& e : events)
      if (e.kind == Event::Kind::Trade) out.push_back({e.maker, e.id, e.price, e.qty});
    return out;
  }
  void clear() { events.clear(); }
};

}  // namespace lob::test
