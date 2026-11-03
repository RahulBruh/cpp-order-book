// Randomized differential test: drive the same random operation stream into
// MapOrderBook, ArrayOrderBook and the naive ReferenceBook, and require
// identical event streams after every operation and identical book state at
// regular checkpoints.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "lob/order_book.hpp"
#include "recording_listener.hpp"
#include "reference_book.hpp"

using namespace lob;
using lob::test::RecordingListener;
using lob::test::ReferenceBook;

namespace {

struct Scenario {
  const char* name;
  std::uint64_t seed;
  int ops;
  Price min_price;
  Price max_price;
  std::size_t max_orders;
  int spread;  // how far from mid new orders land; small = lots of crossing
};

// Readable test names in failure output instead of a hex dump.
inline void PrintTo(const Scenario& s, std::ostream* os) { *os << s.name << " (seed " << s.seed << ")"; }

class Differential : public ::testing::TestWithParam<Scenario> {};

TEST_P(Differential, AllBooksAgree) {
  const Scenario s = GetParam();
  const Config cfg{.min_price = s.min_price, .max_price = s.max_price, .max_orders = s.max_orders};

  RecordingListener lm, la, lr;
  MapOrderBook<RecordingListener> map_book(cfg, lm);
  ArrayOrderBook<RecordingListener> arr_book(cfg, la);
  ReferenceBook<RecordingListener> ref_book(cfg, lr);

  std::mt19937_64 rng(s.seed);
  auto uniform = [&](std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  };

  std::vector<OrderId> issued;  // every id ever sent; many are dead by now
  OrderId next_id = 1;
  const Price mid_lo = s.min_price + s.spread, mid_hi = s.max_price - s.spread;
  Price mid = (s.min_price + s.max_price) / 2;

  auto pick_id = [&]() -> OrderId {
    if (issued.empty()) return 1;
    const auto i = uniform(0, static_cast<std::int64_t>(issued.size()) - 1);
    return issued[static_cast<std::size_t>(i)];
  };

  std::size_t trades = 0, rejects = 0;
  for (int op = 0; op < s.ops; ++op) {
    mid = std::clamp<Price>(mid + uniform(-1, 1), mid_lo, mid_hi);
    const int dice = static_cast<int>(uniform(0, 99));
    const Side side = uniform(0, 1) ? Side::Buy : Side::Sell;
    const Price px = mid + uniform(-s.spread, s.spread);
    const Qty qty = uniform(1, 20);

    auto apply = [&](auto&& fn) {
      fn(map_book);
      fn(arr_book);
      fn(ref_book);
    };

    if (dice < 45) {
      const OrderId id = next_id++;
      issued.push_back(id);
      apply([&](auto& b) { b.add(id, side, OrderType::Limit, px, qty); });
    } else if (dice < 52) {
      const OrderId id = next_id++;
      issued.push_back(id);
      apply([&](auto& b) { b.add(id, side, OrderType::IOC, px, qty); });
    } else if (dice < 55) {
      const OrderId id = next_id++;
      issued.push_back(id);
      apply([&](auto& b) { b.add(id, side, OrderType::Market, 0, qty); });
    } else if (dice < 80) {
      const OrderId id = pick_id();
      apply([&](auto& b) { b.cancel(id); });
    } else if (dice < 95) {
      const OrderId id = pick_id();
      // mostly small reprices/resizes, sometimes in-place reductions
      const Price new_px = uniform(0, 2) ? px : mid;
      const Qty new_qty = uniform(0, 3) ? qty : uniform(0, 2);
      apply([&](auto& b) { b.modify(id, new_px, new_qty); });
    } else {
      // invalid input: zero id, reused live id, out-of-range price, zero qty
      const int kind = static_cast<int>(uniform(0, 3));
      const OrderId id = kind == 0 ? 0 : kind == 1 ? pick_id() : next_id;
      const Price bad_px = kind == 2 ? s.max_price + uniform(1, 5) : px;
      const Qty bad_qty = kind == 3 ? 0 : qty;
      apply([&](auto& b) { b.add(id, side, OrderType::Limit, bad_px, bad_qty); });
    }

    ASSERT_EQ(lm.events, lr.events) << "map book diverged at op " << op;
    ASSERT_EQ(la.events, lr.events) << "array book diverged at op " << op;
    for (const auto& e : lr.events) {
      trades += e.kind == test::Event::Kind::Trade;
      rejects += e.kind == test::Event::Kind::Rejected;
    }
    lm.clear();
    la.clear();
    lr.clear();

    if (op % 64 == 0 || op == s.ops - 1) {
      for (Side sd : {Side::Buy, Side::Sell}) {
        const auto want = ref_book.depth(sd);
        ASSERT_EQ(map_book.depth(sd), want) << "op " << op;
        ASSERT_EQ(arr_book.depth(sd), want) << "op " << op;
      }
      ASSERT_EQ(map_book.order_count(), ref_book.order_count());
      ASSERT_EQ(arr_book.order_count(), ref_book.order_count());
      ASSERT_EQ(map_book.best_bid(), ref_book.best_bid());
      ASSERT_EQ(arr_book.best_ask(), ref_book.best_ask());
    }
  }
  // Make sure the stream actually exercised matching and the reject paths.
  EXPECT_GT(trades, static_cast<std::size_t>(s.ops) / 20);
  EXPECT_GT(rejects, 0u);
  RecordProperty("trades", static_cast<int>(trades));
}

// ~1.15M random operations in total across the scenarios.
INSTANTIATE_TEST_SUITE_P(
    Scenarios, Differential,
    ::testing::Values(
        Scenario{"tight_spread", 1, 250'000, 1, 400, 4096, 5},
        Scenario{"wide_spread", 2, 250'000, 1, 5'000, 4096, 120},
        Scenario{"tiny_pool", 3, 150'000, 1, 300, 24, 6},  // hits PoolExhausted often
        Scenario{"bitset_word_edges", 4, 250'000, 0, 1'023, 4096, 70},
        Scenario{"seed_5", 5, 250'000, 1, 2'000, 2048, 30}),
    [](const auto& param_info) { return std::string(param_info.param.name); });

}  // namespace
