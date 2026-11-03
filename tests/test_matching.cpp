// Behavioural tests for the matching rules, run against both book
// implementations via a typed test suite.

#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "recording_listener.hpp"

using namespace lob;
using lob::test::Event;
using lob::test::RecordingListener;
using K = Event::Kind;

namespace {

constexpr Config kCfg{.min_price = 1, .max_price = 1000, .max_orders = 64};

template <class Levels>
class MatchingTest : public ::testing::Test {
 protected:
  RecordingListener l;
  OrderBook<Levels, RecordingListener> book{kCfg, l};

  void buy(OrderId id, Price px, Qty q, OrderType t = OrderType::Limit) { book.add(id, Side::Buy, t, px, q); }
  void sell(OrderId id, Price px, Qty q, OrderType t = OrderType::Limit) { book.add(id, Side::Sell, t, px, q); }
  std::vector<Trade> trades() { return l.trades(); }
  Event last() { return l.events.back(); }
};

using Impls = ::testing::Types<MapLevels, ArrayLevels>;
TYPED_TEST_SUITE(MatchingTest, Impls);

TYPED_TEST(MatchingTest, EmptyBook) {
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(MatchingTest, NonCrossingOrdersRest) {
  this->buy(1, 99, 10);
  this->sell(2, 101, 5);
  EXPECT_EQ(this->book.best_bid(), 99);
  EXPECT_EQ(this->book.best_ask(), 101);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 99), 10);
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 5);
  EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(MatchingTest, BestPricesTrackAcrossLevels) {
  this->buy(1, 95, 1);
  this->buy(2, 98, 1);
  this->buy(3, 97, 1);
  this->sell(4, 105, 1);
  this->sell(5, 102, 1);
  EXPECT_EQ(this->book.best_bid(), 98);
  EXPECT_EQ(this->book.best_ask(), 102);
  this->book.cancel(2);
  EXPECT_EQ(this->book.best_bid(), 97);
  this->book.cancel(5);
  EXPECT_EQ(this->book.best_ask(), 105);
}

TYPED_TEST(MatchingTest, FullFillAtMakerPrice) {
  this->sell(1, 100, 10);
  this->buy(2, 105, 10);  // aggressive buyer pays the resting price, not its limit
  ASSERT_EQ(this->trades().size(), 1u);
  EXPECT_EQ(this->trades()[0], (Trade{1, 2, 100, 10}));
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_FALSE(this->book.best_bid());
}

TYPED_TEST(MatchingTest, PartialFillRestsRemainder) {
  this->sell(1, 100, 4);
  this->buy(2, 100, 10);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 2, 100, 4}}));
  EXPECT_EQ(this->book.best_bid(), 100);
  EXPECT_EQ(this->book.order_qty(2), 6);
  EXPECT_FALSE(this->book.best_ask());
}

TYPED_TEST(MatchingTest, PartialFillOfMakerKeepsItAtFront) {
  this->sell(1, 100, 10);
  this->sell(2, 100, 10);
  this->buy(3, 100, 4);
  EXPECT_EQ(this->book.order_qty(1), 6);
  EXPECT_EQ(this->book.queue_at(Side::Sell, 100), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(this->book.volume_at(Side::Sell, 100), 16);
}

TYPED_TEST(MatchingTest, TimePriorityWithinLevel) {
  this->sell(1, 100, 5);
  this->sell(2, 100, 5);
  this->sell(3, 100, 5);
  this->buy(4, 100, 12);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 4, 100, 5}, {2, 4, 100, 5}, {3, 4, 100, 2}}));
  EXPECT_EQ(this->book.order_qty(3), 3);
}

TYPED_TEST(MatchingTest, PricePriorityAcrossLevels) {
  this->sell(1, 103, 5);
  this->sell(2, 101, 5);
  this->sell(3, 102, 5);
  this->buy(4, 102, 12);  // sweeps 101 then 102, never touches 103
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{2, 4, 101, 5}, {3, 4, 102, 5}}));
  EXPECT_EQ(this->book.best_bid(), 102);
  EXPECT_EQ(this->book.order_qty(4), 2);
  EXPECT_EQ(this->book.best_ask(), 103);
}

TYPED_TEST(MatchingTest, SellSideSweepsBidsHighestFirst) {
  this->buy(1, 98, 5);
  this->buy(2, 100, 5);
  this->buy(3, 99, 5);
  this->sell(4, 99, 20);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{2, 4, 100, 5}, {3, 4, 99, 5}}));
  EXPECT_EQ(this->book.best_ask(), 99);
  EXPECT_EQ(this->book.best_bid(), 98);
}

TYPED_TEST(MatchingTest, MarketOrderIgnoresPriceAndCancelsRemainder) {
  this->sell(1, 100, 5);
  this->sell(2, 900, 5);
  this->buy(3, 0, 20, OrderType::Market);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 3, 100, 5}, {2, 3, 900, 5}}));
  EXPECT_EQ(this->last(), (Event{K::Cancelled, 3, 0, 0, 10}));
  EXPECT_FALSE(this->book.best_bid());  // market orders never rest
}

TYPED_TEST(MatchingTest, MarketOrderIntoEmptyBook) {
  this->sell(1, 0, 5, OrderType::Market);
  ASSERT_EQ(this->l.events.size(), 2u);
  EXPECT_EQ(this->l.events[0].kind, K::Accepted);
  EXPECT_EQ(this->l.events[1], (Event{K::Cancelled, 1, 0, 0, 5}));
}

TYPED_TEST(MatchingTest, IocRespectsLimitAndNeverRests) {
  this->sell(1, 100, 5);
  this->sell(2, 102, 5);
  this->buy(3, 101, 20, OrderType::IOC);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 3, 100, 5}}));
  EXPECT_EQ(this->last(), (Event{K::Cancelled, 3, 0, 0, 15}));
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_EQ(this->book.best_ask(), 102);
}

TYPED_TEST(MatchingTest, CancelRemovesOrderAndEmptyLevel) {
  this->buy(1, 100, 5);
  this->buy(2, 100, 7);
  this->book.cancel(1);
  EXPECT_EQ(this->last(), (Event{K::Cancelled, 1, 0, 0, 5}));
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 7);
  this->book.cancel(2);
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(MatchingTest, CancelFromMiddleOfQueue) {
  this->buy(1, 100, 1);
  this->buy(2, 100, 1);
  this->buy(3, 100, 1);
  this->book.cancel(2);
  EXPECT_EQ(this->book.queue_at(Side::Buy, 100), (std::vector<OrderId>{1, 3}));
}

TYPED_TEST(MatchingTest, ModifyReduceKeepsPriority) {
  this->buy(1, 100, 10);
  this->buy(2, 100, 10);
  this->book.modify(1, 100, 4);
  EXPECT_EQ(this->last().kind, K::Modified);
  EXPECT_EQ(this->book.queue_at(Side::Buy, 100), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 14);
}

TYPED_TEST(MatchingTest, ModifyIncreaseLosesPriority) {
  this->buy(1, 100, 10);
  this->buy(2, 100, 10);
  this->book.modify(1, 100, 11);
  EXPECT_EQ(this->book.queue_at(Side::Buy, 100), (std::vector<OrderId>{2, 1}));
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 21);
}

TYPED_TEST(MatchingTest, ModifyPriceMovesLevelAndLosesPriority) {
  this->buy(1, 99, 10);
  this->buy(2, 100, 10);
  this->book.modify(2, 99, 10);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 0);
  EXPECT_EQ(this->book.best_bid(), 99);
  EXPECT_EQ(this->book.queue_at(Side::Buy, 99), (std::vector<OrderId>{1, 2}));
}

TYPED_TEST(MatchingTest, ModifyIntoCrossTradesImmediately) {
  this->sell(1, 101, 5);
  this->buy(2, 99, 8);
  this->book.modify(2, 101, 8);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 2, 101, 5}}));
  EXPECT_EQ(this->book.best_bid(), 101);
  EXPECT_EQ(this->book.order_qty(2), 3);
}

TYPED_TEST(MatchingTest, Rejections) {
  auto reason = [&] { return this->last().reason; };
  this->buy(0, 100, 1);
  EXPECT_EQ(reason(), RejectReason::InvalidId);
  this->buy(1, 100, 0);
  EXPECT_EQ(reason(), RejectReason::InvalidQty);
  this->buy(1, 0, 1);
  EXPECT_EQ(reason(), RejectReason::InvalidPrice);
  this->buy(1, 1001, 1);
  EXPECT_EQ(reason(), RejectReason::InvalidPrice);
  this->buy(1, 100, 1);
  this->sell(1, 200, 1);
  EXPECT_EQ(reason(), RejectReason::DuplicateId);
  this->book.cancel(42);
  EXPECT_EQ(reason(), RejectReason::UnknownId);
  this->book.modify(42, 100, 1);
  EXPECT_EQ(reason(), RejectReason::UnknownId);
  this->book.modify(1, 100, 0);
  EXPECT_EQ(reason(), RejectReason::InvalidQty);
  this->book.modify(1, 5000, 1);
  EXPECT_EQ(reason(), RejectReason::InvalidPrice);
  EXPECT_EQ(this->book.order_count(), 1u);  // rejected ops changed nothing
}

TYPED_TEST(MatchingTest, IdReusableAfterFillOrCancel) {
  this->sell(1, 100, 5);
  this->buy(2, 100, 5);
  this->sell(1, 100, 5);  // id 1 fully filled earlier, so reuse is allowed
  EXPECT_EQ(this->last().kind, K::Accepted);
  this->book.cancel(1);
  this->sell(1, 100, 5);
  EXPECT_EQ(this->last().kind, K::Accepted);
}

TYPED_TEST(MatchingTest, PoolExhaustionRejectsOnlyRestingOrders) {
  RecordingListener sl;
  OrderBook<TypeParam, RecordingListener> small({.min_price = 1, .max_price = 1000, .max_orders = 2}, sl);
  small.add(1, Side::Sell, OrderType::Limit, 100, 5);
  small.add(2, Side::Sell, OrderType::Limit, 101, 5);
  small.add(3, Side::Sell, OrderType::Limit, 102, 5);
  EXPECT_EQ(sl.events.back().reason, RejectReason::PoolExhausted);
  small.add(4, Side::Buy, OrderType::IOC, 100, 5);  // IOC never needs a slot
  EXPECT_EQ(sl.trades().size(), 1u);
  small.add(5, Side::Sell, OrderType::Limit, 102, 5);  // slot freed by the fill
  EXPECT_EQ(sl.events.back().kind, K::Accepted);
}

TYPED_TEST(MatchingTest, PricesAtRangeBoundaries) {
  this->buy(1, 1, 5);
  this->sell(2, 1000, 5);
  EXPECT_EQ(this->book.best_bid(), 1);
  EXPECT_EQ(this->book.best_ask(), 1000);
  this->sell(3, 1, 5);
  EXPECT_EQ(this->trades(), (std::vector<Trade>{{1, 3, 1, 5}}));
  this->buy(4, 1000, 5);
  EXPECT_EQ(this->trades().back(), (Trade{2, 4, 1000, 5}));
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(MatchingTest, DepthSnapshotIsBestFirst) {
  this->buy(1, 97, 1);
  this->buy(2, 99, 2);
  this->buy(3, 99, 3);
  this->buy(4, 98, 4);
  using D = std::vector<std::pair<Price, Qty>>;
  EXPECT_EQ(this->book.depth(Side::Buy), (D{{99, 5}, {98, 4}, {97, 1}}));
  EXPECT_EQ(this->book.depth(Side::Buy, 2), (D{{99, 5}, {98, 4}}));
}

// Levels far apart exercise the multi-word bitset scan in ArrayLevels.
TEST(ArrayLevelsTest, BestPriceScansAcrossBitsetWords) {
  RecordingListener l;
  ArrayOrderBook<RecordingListener> book({.min_price = 0, .max_price = 100'000, .max_orders = 16}, l);
  book.add(1, Side::Buy, OrderType::Limit, 3, 1);
  book.add(2, Side::Buy, OrderType::Limit, 64, 1);
  book.add(3, Side::Buy, OrderType::Limit, 63, 1);
  book.add(4, Side::Buy, OrderType::Limit, 60'000, 1);
  book.add(5, Side::Sell, OrderType::Limit, 100'000, 1);
  book.add(6, Side::Sell, OrderType::Limit, 100'000 - 64, 1);
  book.cancel(4);
  EXPECT_EQ(book.best_bid(), 64);
  book.cancel(2);
  EXPECT_EQ(book.best_bid(), 63);
  book.cancel(3);
  EXPECT_EQ(book.best_bid(), 3);
  book.cancel(1);
  EXPECT_FALSE(book.best_bid());
  EXPECT_EQ(book.best_ask(), 100'000 - 64);
  book.cancel(6);
  EXPECT_EQ(book.best_ask(), 100'000);
}

}  // namespace
