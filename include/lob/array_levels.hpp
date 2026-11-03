#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Price levels in a flat array indexed by tick: level for price p lives at
// levels[p - min_price]. Everything is allocated in the constructor.
//
// get_or_create: O(1) (index + set a bit).
// best():        O(1) (cached index).
// remove():      O(1) unless it empties the best level; then the next best
//                level is found by scanning an occupancy bitset 64 ticks per
//                machine word (count-leading/trailing-zeros), so a gap of G
//                empty ticks costs O(G / 64) word reads.
//
// Trade-off vs MapLevels: memory is proportional to the price range, not to
// the number of live levels, so the range must be bounded up front.
class ArrayLevels {
 public:
  explicit ArrayLevels(const Config& cfg) : min_price_(cfg.min_price) {
    if (cfg.max_price < cfg.min_price) throw std::invalid_argument("max_price < min_price");
    n_ = static_cast<std::size_t>(cfg.max_price - cfg.min_price) + 1;
    for (SideBook* sb : {&bids_, &asks_}) {
      sb->levels.resize(n_);
      for (std::size_t i = 0; i < n_; ++i) sb->levels[i].price = min_price_ + static_cast<Price>(i);
      sb->occupied.assign((n_ + 63) / 64, 0);
    }
  }

  PriceLevel* get_or_create(Side side, Price price) noexcept {
    SideBook& sb = book(side);
    const auto idx = static_cast<std::ptrdiff_t>(price - min_price_);
    PriceLevel& level = sb.levels[static_cast<std::size_t>(idx)];
    if (level.empty()) {
      set_bit(sb.occupied, static_cast<std::size_t>(idx));
      if (sb.best < 0 || (side == Side::Buy ? idx > sb.best : idx < sb.best)) sb.best = idx;
    }
    return &level;
  }

  void remove(Side side, PriceLevel* level) noexcept {
    SideBook& sb = book(side);
    const std::ptrdiff_t idx = level - sb.levels.data();
    clear_bit(sb.occupied, static_cast<std::size_t>(idx));
    if (idx == sb.best) {
      sb.best = side == Side::Buy ? highest_below(sb.occupied, static_cast<std::size_t>(idx))
                                  : lowest_above(sb.occupied, static_cast<std::size_t>(idx));
    }
  }

  PriceLevel* best(Side side) noexcept {
    SideBook& sb = book(side);
    return sb.best < 0 ? nullptr : &sb.levels[static_cast<std::size_t>(sb.best)];
  }

  const PriceLevel* find(Side side, Price price) const noexcept {
    if (price < min_price_ || price >= min_price_ + static_cast<Price>(n_)) return nullptr;
    const PriceLevel& level = book(side).levels[static_cast<std::size_t>(price - min_price_)];
    return level.empty() ? nullptr : &level;
  }

  template <class Fn>
  void for_each_level(Side side, Fn&& fn) const {
    const SideBook& sb = book(side);
    for (std::ptrdiff_t i = sb.best; i >= 0;) {
      if (!fn(sb.levels[static_cast<std::size_t>(i)])) return;
      i = side == Side::Buy ? highest_below(sb.occupied, static_cast<std::size_t>(i))
                            : lowest_above(sb.occupied, static_cast<std::size_t>(i));
    }
  }

 private:
  struct SideBook {
    std::vector<PriceLevel> levels;
    std::vector<std::uint64_t> occupied;  // bit i set <=> levels[i] non-empty
    std::ptrdiff_t best = -1;             // index of best level, -1 if side empty
  };

  SideBook& book(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  const SideBook& book(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

  static void set_bit(std::vector<std::uint64_t>& bits, std::size_t i) noexcept {
    bits[i >> 6] |= std::uint64_t{1} << (i & 63);
  }
  static void clear_bit(std::vector<std::uint64_t>& bits, std::size_t i) noexcept {
    bits[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
  }

  // Highest set index strictly below i, or -1.
  static std::ptrdiff_t highest_below(const std::vector<std::uint64_t>& bits, std::size_t i) noexcept {
    if (i == 0) return -1;
    --i;
    std::size_t w = i >> 6;
    const unsigned b = static_cast<unsigned>(i & 63);
    std::uint64_t word = bits[w] & (b == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (b + 1)) - 1);
    for (;;) {
      if (word) return static_cast<std::ptrdiff_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(word)));
      if (w == 0) return -1;
      word = bits[--w];
    }
  }

  // Lowest set index strictly above i, or -1.
  static std::ptrdiff_t lowest_above(const std::vector<std::uint64_t>& bits, std::size_t i) noexcept {
    ++i;
    std::size_t w = i >> 6;
    if (w >= bits.size()) return -1;
    std::uint64_t word = bits[w] & (~std::uint64_t{0} << (i & 63));
    for (;;) {
      if (word) return static_cast<std::ptrdiff_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(word)));
      if (++w == bits.size()) return -1;
      word = bits[w];
    }
  }

  Price min_price_;
  std::size_t n_ = 0;
  SideBook bids_;
  SideBook asks_;
};

}  // namespace lob
