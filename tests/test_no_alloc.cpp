// Verifies the claim that ArrayOrderBook never touches the heap after
// construction, by counting calls to global operator new while it processes
// a long random stream. Lives in its own executable because it replaces the
// global allocation functions.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>

#include "lob/order_book.hpp"

namespace {
std::atomic<bool> g_counting{false};
std::atomic<long> g_allocs{0};
}  // namespace

void* operator new(std::size_t n) {
  if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// Counts events without storing them (storing would allocate).
struct CountingListener {
  long accepted = 0, rejected = 0, trades = 0, cancelled = 0, modified = 0;
  void on_accepted(lob::OrderId) { ++accepted; }
  void on_rejected(lob::OrderId, lob::RejectReason) { ++rejected; }
  void on_trade(const lob::Trade&) { ++trades; }
  void on_cancelled(lob::OrderId, lob::Qty) { ++cancelled; }
  void on_modified(lob::OrderId) { ++modified; }
};

template <class Levels>
long allocations_during_random_stream() {
  using namespace lob;
  CountingListener l;
  OrderBook<Levels, CountingListener> book({.min_price = 1, .max_price = 10'000, .max_orders = 4096}, l);
  std::mt19937_64 rng(11);

  g_allocs = 0;
  g_counting = true;
  OrderId next = 1;
  for (int i = 0; i < 300'000; ++i) {
    const Side side = rng() % 2 ? Side::Buy : Side::Sell;
    const Price px = 5'000 + static_cast<Price>(rng() % 200) - 100;
    const Qty q = 1 + static_cast<Qty>(rng() % 50);
    switch (rng() % 10) {
      case 0: book.add(next++, side, OrderType::IOC, px, q); break;
      case 1: book.add(next++, side, OrderType::Market, 0, q); break;
      case 2: case 3: case 4: book.cancel(1 + rng() % next); break;
      case 5: book.modify(1 + rng() % next, px, q); break;
      default: book.add(next++, side, OrderType::Limit, px, q); break;
    }
  }
  g_counting = false;
  EXPECT_GT(l.trades, 10'000);
  EXPECT_GT(l.cancelled, 10'000);
  return g_allocs.load();
}

TEST(NoAllocation, ArrayBookHotPathNeverAllocates) {
  EXPECT_EQ(allocations_during_random_stream<lob::ArrayLevels>(), 0);
}

// Sanity check that the counter works: the map book allocates a tree node
// every time a new price level appears.
TEST(NoAllocation, MapBookDoesAllocateNewLevels) {
  EXPECT_GT(allocations_during_random_stream<lob::MapLevels>(), 0);
}

}  // namespace
