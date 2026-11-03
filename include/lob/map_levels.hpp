#pragma once

#include <functional>
#include <map>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Price levels in two balanced trees (red-black, via std::map).
// best(): O(1) (begin()); get_or_create/remove: O(log L) for L levels,
// plus a heap allocation whenever a new price level appears.
// Works for any price range; this is the baseline implementation.
class MapLevels {
 public:
  explicit MapLevels(const Config&) {}

  PriceLevel* get_or_create(Side side, Price price) {
    return side == Side::Buy ? get_or_create(bids_, price) : get_or_create(asks_, price);
  }

  // Called by the book once a level has no orders left.
  void remove(Side side, PriceLevel* level) {
    if (side == Side::Buy) {
      bids_.erase(level->price);
    } else {
      asks_.erase(level->price);
    }
  }

  PriceLevel* best(Side side) noexcept {
    if (side == Side::Buy) return bids_.empty() ? nullptr : &bids_.begin()->second;
    return asks_.empty() ? nullptr : &asks_.begin()->second;
  }

  const PriceLevel* find(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
      auto it = bids_.find(price);
      return it == bids_.end() ? nullptr : &it->second;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? nullptr : &it->second;
  }

  // Visits levels from best to worst; stops early if fn returns false.
  template <class Fn>
  void for_each_level(Side side, Fn&& fn) const {
    auto walk = [&](const auto& m) {
      for (const auto& [price, level] : m)
        if (!fn(level)) return;
    };
    side == Side::Buy ? walk(bids_) : walk(asks_);
  }

 private:
  template <class M>
  static PriceLevel* get_or_create(M& m, Price price) {
    auto [it, inserted] = m.try_emplace(price);
    if (inserted) it->second.price = price;
    return &it->second;  // std::map nodes never move, so this pointer stays valid
  }

  std::map<Price, PriceLevel, std::greater<Price>> bids_;  // highest first
  std::map<Price, PriceLevel, std::less<Price>> asks_;     // lowest first
};

}  // namespace lob
