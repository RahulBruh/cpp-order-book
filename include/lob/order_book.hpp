#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <utility>
#include <vector>

#include "lob/array_levels.hpp"
#include "lob/flat_id_map.hpp"
#include "lob/map_levels.hpp"
#include "lob/object_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// What the book reports. Callbacks are invoked synchronously, in order, from
// inside add/cancel/modify. Taking the listener as a template parameter lets
// the compiler inline the calls (no virtual dispatch on the hot path).
template <class L>
concept Listener = requires(L l, OrderId id, Qty q, RejectReason r, const Trade& t) {
  l.on_accepted(id);
  l.on_rejected(id, r);
  l.on_trade(t);
  l.on_cancelled(id, q);  // explicit cancel, or unfilled remainder of Market/IOC
  l.on_modified(id);
};

// Single-instrument limit order book with price-time priority.
//
// `Levels` decides how price levels are stored (MapLevels or ArrayLevels);
// all matching logic lives here and is shared, so both implementations must
// produce identical event streams (checked by the differential test).
template <class Levels, Listener L>
class OrderBook {
 public:
  OrderBook(const Config& cfg, L& listener)
      : cfg_(cfg), levels_(cfg), pool_(cfg.max_orders), ids_(cfg.max_orders), listener_(listener) {}

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // New order. `price` is ignored for Market orders.
  void add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    if (id == 0) return listener_.on_rejected(id, RejectReason::InvalidId);
    if (qty <= 0) return listener_.on_rejected(id, RejectReason::InvalidQty);
    if (type != OrderType::Market && !valid_price(price)) return listener_.on_rejected(id, RejectReason::InvalidPrice);
    if (ids_.contains(id)) return listener_.on_rejected(id, RejectReason::DuplicateId);
    // Checked up front so an order is never partially filled and then rejected.
    if (type == OrderType::Limit && pool_.available() == 0) return listener_.on_rejected(id, RejectReason::PoolExhausted);

    listener_.on_accepted(id);
    const Qty remaining = match(id, side, type, price, qty);
    if (remaining == 0) return;
    if (type == OrderType::Limit) {
      rest(id, side, price, remaining);
    } else {
      listener_.on_cancelled(id, remaining);
    }
  }

  void cancel(OrderId id) {
    Order* o = ids_.find(id);
    if (!o) return listener_.on_rejected(id, RejectReason::UnknownId);
    const Qty remaining = o->qty;
    remove(o);
    listener_.on_cancelled(id, remaining);
  }

  // Change a resting order's price and/or remaining quantity.
  // Same price and quantity not increased: amended in place, keeps its queue
  // position. Anything else (new price, or more quantity): loses time
  // priority, as on most exchanges. A new price may cross and trade at once.
  void modify(OrderId id, Price new_price, Qty new_qty) {
    Order* o = ids_.find(id);
    if (!o) return listener_.on_rejected(id, RejectReason::UnknownId);
    if (new_qty <= 0) return listener_.on_rejected(id, RejectReason::InvalidQty);
    if (!valid_price(new_price)) return listener_.on_rejected(id, RejectReason::InvalidPrice);

    if (new_price == o->price && new_qty <= o->qty) {
      o->level->total_qty -= o->qty - new_qty;
      o->qty = new_qty;
      return listener_.on_modified(id);
    }

    const Side side = o->side;
    remove(o);  // frees a pool slot, so re-resting below cannot fail
    listener_.on_modified(id);
    const Qty remaining = match(id, side, OrderType::Limit, new_price, new_qty);
    if (remaining > 0) rest(id, side, new_price, remaining);
  }

  // ---- queries -----------------------------------------------------------

  std::optional<Price> best_bid() noexcept { return best_price(Side::Buy); }
  std::optional<Price> best_ask() noexcept { return best_price(Side::Sell); }

  Qty volume_at(Side side, Price price) const noexcept {
    const PriceLevel* level = levels_.find(side, price);
    return level ? level->total_qty : 0;
  }

  bool contains(OrderId id) const noexcept { return ids_.contains(id); }

  // The resting order with this id, or nullptr. Read-only view.
  const Order* find_order(OrderId id) const noexcept { return ids_.find(id); }

  // Remaining quantity of a resting order, or 0 if not resting.
  Qty order_qty(OrderId id) const noexcept {
    const Order* o = ids_.find(id);
    return o ? o->qty : 0;
  }

  std::size_t order_count() const noexcept { return pool_.in_use(); }

  // Up to `max_levels` (price, total qty) pairs from best to worst.
  // Allocates; meant for tests and inspection, not the hot path.
  std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t max_levels = SIZE_MAX) const {
    std::vector<std::pair<Price, Qty>> out;
    levels_.for_each_level(side, [&](const PriceLevel& level) {
      if (out.size() >= max_levels) return false;
      out.emplace_back(level.price, level.total_qty);
      return true;
    });
    return out;
  }

  // Order ids at one price in queue order (tests use this to check priority).
  std::vector<OrderId> queue_at(Side side, Price price) const {
    std::vector<OrderId> out;
    if (const PriceLevel* level = levels_.find(side, price))
      for (const Order* o = level->head; o; o = o->next) out.push_back(o->id);
    return out;
  }

 private:
  bool valid_price(Price p) const noexcept { return p >= cfg_.min_price && p <= cfg_.max_price; }

  static bool crosses(Side taker_side, Price limit, Price level_price) noexcept {
    return taker_side == Side::Buy ? level_price <= limit : level_price >= limit;
  }

  std::optional<Price> best_price(Side side) noexcept {
    const PriceLevel* level = levels_.best(side);
    return level ? std::optional<Price>(level->price) : std::nullopt;
  }

  // Walk the opposite side from its best price, filling the oldest order at
  // each level first. Returns the taker's unfilled quantity.
  Qty match(OrderId taker, Side side, OrderType type, Price limit, Qty qty) {
    const Side opp = opposite(side);
    while (qty > 0) {
      PriceLevel* level = levels_.best(opp);
      if (!level) break;
      if (type != OrderType::Market && !crosses(side, limit, level->price)) break;

      while (qty > 0 && level->head) {
        Order* maker = level->head;
        const Qty fill = std::min(qty, maker->qty);
        maker->qty -= fill;
        level->total_qty -= fill;
        qty -= fill;
        listener_.on_trade(Trade{maker->id, taker, level->price, fill});
        if (maker->qty == 0) {
          level->erase(maker);
          ids_.erase(maker->id);
          pool_.release(maker);
        }
      }
      if (level->empty()) levels_.remove(opp, level);
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    Order* o = pool_.acquire();  // availability was checked by the caller
    o->id = id;
    o->side = side;
    o->price = price;
    o->qty = qty;
    levels_.get_or_create(side, price)->push_back(o);
    ids_.insert(id, o);
  }

  void remove(Order* o) {
    PriceLevel* level = o->level;
    level->erase(o);
    if (level->empty()) levels_.remove(o->side, level);
    ids_.erase(o->id);
    pool_.release(o);
  }

  Config cfg_;
  Levels levels_;
  ObjectPool<Order> pool_;
  FlatIdMap<Order*> ids_;
  L& listener_;
};

template <Listener L>
using MapOrderBook = OrderBook<MapLevels, L>;

template <Listener L>
using ArrayOrderBook = OrderBook<ArrayLevels, L>;

}  // namespace lob
