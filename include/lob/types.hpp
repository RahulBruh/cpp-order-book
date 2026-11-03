#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;  // 0 is reserved as "no order"
using Price = std::int64_t;     // integer ticks; never floating point
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

// Limit: match what crosses, rest the remainder (good-till-cancel).
// Market: match at any price, cancel the remainder.
// IOC: match up to the limit price, cancel the remainder (never rests).
enum class OrderType : std::uint8_t { Limit, Market, IOC };

enum class RejectReason : std::uint8_t {
  InvalidId,      // id == 0
  InvalidQty,     // qty <= 0
  InvalidPrice,   // outside the book's configured tick range
  DuplicateId,    // id already resting in the book
  UnknownId,      // cancel/modify of an id that is not resting
  PoolExhausted,  // a resting limit order would exceed max_orders
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct Trade {
  OrderId maker;  // resting order
  OrderId taker;  // incoming order
  Price price;    // always the maker's price
  Qty qty;
  friend bool operator==(const Trade&, const Trade&) = default;
};

struct Config {
  Price min_price = 0;              // lowest valid tick (inclusive)
  Price max_price = 0;              // highest valid tick (inclusive)
  std::size_t max_orders = 1 << 20; // capacity of the resting-order pool
};

}  // namespace lob
