#pragma once

// A deliberately naive order book used only as a test oracle.
//
// Every resting order sits in one flat vector; each operation finds the best
// order by scanning all of them (O(n)). There are no price levels, no linked
// lists, no hashing: the rules are written as directly as possible, so if
// the fast books ever disagree with this one, the fast books are wrong.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "lob/types.hpp"

namespace lob::test {

template <class L>
class ReferenceBook {
 public:
  ReferenceBook(const Config& cfg, L& listener) : cfg_(cfg), listener_(listener) {}

  void add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    if (id == 0) return listener_.on_rejected(id, RejectReason::InvalidId);
    if (qty <= 0) return listener_.on_rejected(id, RejectReason::InvalidQty);
    if (type != OrderType::Market && !valid_price(price)) return listener_.on_rejected(id, RejectReason::InvalidPrice);
    if (find(id) >= 0) return listener_.on_rejected(id, RejectReason::DuplicateId);
    if (type == OrderType::Limit && orders_.size() >= cfg_.max_orders)
      return listener_.on_rejected(id, RejectReason::PoolExhausted);

    listener_.on_accepted(id);
    Qty remaining = match(id, side, type, price, qty);
    if (remaining == 0) return;
    if (type == OrderType::Limit) {
      orders_.push_back({id, side, price, remaining, next_seq_++});
    } else {
      listener_.on_cancelled(id, remaining);
    }
  }

  void cancel(OrderId id) {
    const std::ptrdiff_t i = find(id);
    if (i < 0) return listener_.on_rejected(id, RejectReason::UnknownId);
    const Qty q = orders_[static_cast<std::size_t>(i)].qty;
    orders_.erase(orders_.begin() + i);
    listener_.on_cancelled(id, q);
  }

  void modify(OrderId id, Price new_price, Qty new_qty) {
    const std::ptrdiff_t i = find(id);
    if (i < 0) return listener_.on_rejected(id, RejectReason::UnknownId);
    if (new_qty <= 0) return listener_.on_rejected(id, RejectReason::InvalidQty);
    if (!valid_price(new_price)) return listener_.on_rejected(id, RejectReason::InvalidPrice);

    RefOrder& o = orders_[static_cast<std::size_t>(i)];
    if (new_price == o.price && new_qty <= o.qty) {
      o.qty = new_qty;  // keeps its seq, i.e. its time priority
      return listener_.on_modified(id);
    }
    const Side side = o.side;
    orders_.erase(orders_.begin() + i);
    listener_.on_modified(id);
    Qty remaining = match(id, side, OrderType::Limit, new_price, new_qty);
    if (remaining > 0) orders_.push_back({id, side, new_price, remaining, next_seq_++});
  }

  std::optional<Price> best_bid() const { return best_price(Side::Buy); }
  std::optional<Price> best_ask() const { return best_price(Side::Sell); }
  std::size_t order_count() const { return orders_.size(); }

  // Same shape as OrderBook::depth(): (price, total qty), best first.
  std::vector<std::pair<Price, Qty>> depth(Side side) const {
    std::vector<std::pair<Price, Qty>> out;
    for (const auto& o : orders_) {
      if (o.side != side) continue;
      bool found = false;
      for (auto& [p, q] : out)
        if (p == o.price) {
          q += o.qty;
          found = true;
        }
      if (!found) out.emplace_back(o.price, o.qty);
    }
    // insertion sort: tiny n, and keeps this file dependency-free
    for (std::size_t i = 1; i < out.size(); ++i)
      for (std::size_t j = i; j > 0 && better(side, out[j].first, out[j - 1].first); --j) std::swap(out[j], out[j - 1]);
    return out;
  }

 private:
  struct RefOrder {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    std::uint64_t seq;  // arrival order; lower = earlier = higher priority
  };

  static bool better(Side side, Price a, Price b) { return side == Side::Buy ? a > b : a < b; }
  bool valid_price(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }

  std::ptrdiff_t find(OrderId id) const {
    for (std::size_t i = 0; i < orders_.size(); ++i)
      if (orders_[i].id == id) return static_cast<std::ptrdiff_t>(i);
    return -1;
  }

  // Index of the highest-priority order on `side`: best price, then earliest.
  std::ptrdiff_t best_index(Side side) const {
    std::ptrdiff_t best = -1;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const auto& o = orders_[i];
      if (o.side != side) continue;
      if (best < 0) {
        best = static_cast<std::ptrdiff_t>(i);
        continue;
      }
      const auto& b = orders_[static_cast<std::size_t>(best)];
      if (better(side, o.price, b.price) || (o.price == b.price && o.seq < b.seq)) best = static_cast<std::ptrdiff_t>(i);
    }
    return best;
  }

  std::optional<Price> best_price(Side side) const {
    const std::ptrdiff_t i = best_index(side);
    if (i < 0) return std::nullopt;
    return orders_[static_cast<std::size_t>(i)].price;
  }

  Qty match(OrderId taker, Side side, OrderType type, Price limit, Qty qty) {
    const Side opp = opposite(side);
    while (qty > 0) {
      const std::ptrdiff_t i = best_index(opp);
      if (i < 0) break;
      RefOrder& maker = orders_[static_cast<std::size_t>(i)];
      const bool crosses = side == Side::Buy ? maker.price <= limit : maker.price >= limit;
      if (type != OrderType::Market && !crosses) break;
      const Qty fill = qty < maker.qty ? qty : maker.qty;
      maker.qty -= fill;
      qty -= fill;
      listener_.on_trade(Trade{maker.id, taker, maker.price, fill});
      if (maker.qty == 0) orders_.erase(orders_.begin() + i);
    }
    return qty;
  }

  Config cfg_;
  L& listener_;
  std::vector<RefOrder> orders_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace lob::test
