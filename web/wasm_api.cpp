// C API over the order book for the browser demo, compiled to WebAssembly
// with wasi-sdk (see build.sh). The engine headers are used unchanged.
//
// Two independent parts:
//  * An interactive book (tick-array levels) that the page drives one order
//    at a time. Each call records the events the book emitted into a buffer
//    the page reads from linear memory.
//  * A benchmark: the same seeded order-flow generator as bench/bench.cpp,
//    replayed into a fresh std::map book or tick-array book. The page times
//    each replay with performance.now().
//
// All arguments are 32-bit so JavaScript can pass plain numbers.

#include <cstdint>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

// The WASI libc++ has no exception runtime. The engine's only throw is the
// ArrayLevels config check (max_price < min_price), which the fixed configs
// below never trigger, so a throw just traps.
extern "C" void* __cxa_allocate_exception(std::size_t) { __builtin_trap(); }
extern "C" void __cxa_throw(void*, void*, void (*)(void*)) { __builtin_trap(); }

namespace {

// ---- interactive book -----------------------------------------------------

// One event = 5 int32s: kind, id, a, b, c.
//   0 accepted   id
//   1 rejected   id, a = RejectReason
//   2 trade      id = maker, a = taker, b = price, c = qty
//   3 cancelled  id, a = cancelled qty
//   4 modified   id
struct Event {
  std::int32_t kind, id, a, b, c;
};

struct RecordingListener {
  std::vector<Event> events;
  void on_accepted(OrderId id) { events.push_back({0, i32(id), 0, 0, 0}); }
  void on_rejected(OrderId id, RejectReason r) { events.push_back({1, i32(id), static_cast<std::int32_t>(r), 0, 0}); }
  void on_trade(const Trade& t) { events.push_back({2, i32(t.maker), i32(t.taker), i32(t.price), i32(t.qty)}); }
  void on_cancelled(OrderId id, Qty q) { events.push_back({3, i32(id), i32(q), 0, 0}); }
  void on_modified(OrderId id) { events.push_back({4, i32(id), 0, 0, 0}); }
  static std::int32_t i32(std::int64_t v) { return static_cast<std::int32_t>(v); }
};

// Prices are integer ticks: 1 tick = $0.01, so 10000 is $100.00.
constexpr Config kLiveCfg{.min_price = 9'000, .max_price = 11'000, .max_orders = 4'096};

RecordingListener g_listener;
ArrayOrderBook<RecordingListener>* g_book = nullptr;
std::vector<std::int32_t> g_out;  // depth / queue results

ArrayOrderBook<RecordingListener>& book() {
  if (!g_book) g_book = new ArrayOrderBook<RecordingListener>(kLiveCfg, g_listener);
  return *g_book;
}

Side side_of(std::int32_t s) { return s == 0 ? Side::Buy : Side::Sell; }

// ---- benchmark (generator copied from bench/bench.cpp) --------------------

enum class Op : std::uint8_t { Add, Cancel, Modify };

struct Msg {
  Op op;
  Side side;
  OrderType type;
  OrderId id;
  Price price;
  Qty qty;
};

struct SinkListener {
  std::uint64_t traded_qty = 0, trades = 0;
  void on_accepted(OrderId) {}
  void on_rejected(OrderId, RejectReason) {}
  void on_trade(const Trade& t) {
    ++trades;
    traded_qty += static_cast<std::uint64_t>(t.qty);
  }
  void on_cancelled(OrderId, Qty) {}
  void on_modified(OrderId) {}
};

constexpr Config kBenchCfg{.min_price = 0, .max_price = 99'999, .max_orders = 1 << 18};
constexpr std::size_t kTargetResting = 5'000;

std::vector<Msg> g_msgs;
double g_avg_resting = 0, g_avg_levels = 0;
SinkListener g_last;

void generate(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::geometric_distribution<int> depth(0.2);
  std::uniform_int_distribution<int> pct(0, 99);
  std::uniform_int_distribution<Qty> size(1, 100);

  SinkListener sink;
  ArrayOrderBook<SinkListener> b(kBenchCfg, sink);
  std::vector<OrderId> live;
  Price mid = 50'000;
  OrderId next_id = 1;
  double resting = 0, levels = 0;
  std::size_t samples = 0;

  auto pick_live = [&]() -> OrderId {
    while (!live.empty()) {
      std::size_t i = rng() % live.size();
      OrderId id = live[i];
      if (b.contains(id)) return id;
      live[i] = live.back();
      live.pop_back();
    }
    return 0;
  };

  g_msgs.clear();
  g_msgs.reserve(n);
  while (g_msgs.size() < n) {
    const auto bid = b.best_bid();
    const auto ask = b.best_ask();
    if (bid && ask) mid = (*bid + *ask) / 2;
    const Side side = rng() % 2 ? Side::Buy : Side::Sell;
    const int sign = side == Side::Buy ? -1 : 1;
    const Price passive = side == Side::Buy ? (ask ? *ask : mid + 1) - 1 : (bid ? *bid : mid - 1) + 1;
    const Price far_touch = side == Side::Buy ? (ask ? *ask : mid) : (bid ? *bid : mid);
    int roll = pct(rng);
    if (roll >= 52 && roll < 92 && b.order_count() < kTargetResting) roll = 0;
    Msg m{};

    if (roll < 45) {
      m = {Op::Add, side, OrderType::Limit, next_id++, passive + sign * depth(rng), size(rng)};
      live.push_back(m.id);
    } else if (roll < 49) {
      m = {Op::Add, side, OrderType::Limit, next_id++, far_touch - sign * depth(rng), size(rng)};
      live.push_back(m.id);
    } else if (roll < 51) {
      m = {Op::Add, side, OrderType::IOC, next_id++, far_touch - sign * depth(rng), size(rng)};
    } else if (roll < 52) {
      m = {Op::Add, side, OrderType::Market, next_id++, 0, size(rng)};
    } else if (roll < 92) {
      OrderId id = pick_live();
      if (!id) continue;
      m = {Op::Cancel, side, OrderType::Limit, id, 0, 0};
    } else {
      OrderId id = pick_live();
      if (!id) continue;
      const Order* o = b.find_order(id);
      const Qty cur = o->qty;
      const Side oside = o->side;
      const bool reduce = rng() % 2 && cur > 1;
      const Price opassive = oside == Side::Buy ? (ask ? *ask : mid + 1) - 1 : (bid ? *bid : mid - 1) + 1;
      Price px = reduce ? o->price : opassive + (oside == Side::Buy ? -1 : 1) * depth(rng);
      Qty q = reduce ? cur / 2 : size(rng);
      m = {Op::Modify, oside, OrderType::Limit, id, px, q};
    }

    switch (m.op) {
      case Op::Add: b.add(m.id, m.side, m.type, m.price, m.qty); break;
      case Op::Cancel: b.cancel(m.id); break;
      case Op::Modify: b.modify(m.id, m.price, m.qty); break;
    }
    g_msgs.push_back(m);
    if (g_msgs.size() % 1024 == 0) {
      resting += static_cast<double>(b.order_count());
      levels += static_cast<double>(b.depth(Side::Buy).size() + b.depth(Side::Sell).size());
      ++samples;
    }
  }
  g_avg_resting = samples ? resting / static_cast<double>(samples) : 0;
  g_avg_levels = samples ? levels / static_cast<double>(samples) : 0;
}

template <class Levels>
void replay() {
  SinkListener sink;
  auto* b = new OrderBook<Levels, SinkListener>(kBenchCfg, sink);
  for (const Msg& m : g_msgs) {
    switch (m.op) {
      case Op::Add: b->add(m.id, m.side, m.type, m.price, m.qty); break;
      case Op::Cancel: b->cancel(m.id); break;
      case Op::Modify: b->modify(m.id, m.price, m.qty); break;
    }
  }
  delete b;
  g_last = sink;
}

}  // namespace

// ---- interactive exports ----------------------------------------------------

EXPORT(ob_reset) void ob_reset() {
  delete g_book;
  g_book = nullptr;
  g_listener.events.clear();
  book();
}

// side: 0 buy, 1 sell. type: 0 limit, 1 market, 2 IOC. Returns the event count.
EXPORT(ob_add) std::int32_t ob_add(std::int32_t id, std::int32_t side, std::int32_t type, std::int32_t price, std::int32_t qty) {
  g_listener.events.clear();
  book().add(static_cast<OrderId>(id), side_of(side), static_cast<OrderType>(type), price, qty);
  return static_cast<std::int32_t>(g_listener.events.size());
}

EXPORT(ob_cancel) std::int32_t ob_cancel(std::int32_t id) {
  g_listener.events.clear();
  book().cancel(static_cast<OrderId>(id));
  return static_cast<std::int32_t>(g_listener.events.size());
}

EXPORT(ob_modify) std::int32_t ob_modify(std::int32_t id, std::int32_t price, std::int32_t qty) {
  g_listener.events.clear();
  book().modify(static_cast<OrderId>(id), price, qty);
  return static_cast<std::int32_t>(g_listener.events.size());
}

EXPORT(ob_events) const Event* ob_events() { return g_listener.events.data(); }

EXPORT(ob_best) std::int32_t ob_best(std::int32_t side) {
  auto p = side == 0 ? book().best_bid() : book().best_ask();
  return p ? static_cast<std::int32_t>(*p) : -1;
}

EXPORT(ob_order_count) std::int32_t ob_order_count() { return static_cast<std::int32_t>(book().order_count()); }

EXPORT(ob_order_qty) std::int32_t ob_order_qty(std::int32_t id) { return static_cast<std::int32_t>(book().order_qty(static_cast<OrderId>(id))); }

// A resting order's price and side (0 buy, 1 sell), or -1 if it isn't resting.
EXPORT(ob_order_price) std::int32_t ob_order_price(std::int32_t id) {
  const Order* o = book().find_order(static_cast<OrderId>(id));
  return o ? static_cast<std::int32_t>(o->price) : -1;
}
EXPORT(ob_order_side) std::int32_t ob_order_side(std::int32_t id) {
  const Order* o = book().find_order(static_cast<OrderId>(id));
  return o ? (o->side == Side::Buy ? 0 : 1) : -1;
}

// Up to max_levels (price, total qty) pairs from best to worst into ob_out(). Returns the pair count.
EXPORT(ob_depth) std::int32_t ob_depth(std::int32_t side, std::int32_t max_levels) {
  g_out.clear();
  for (auto [p, q] : book().depth(side_of(side), static_cast<std::size_t>(max_levels))) {
    g_out.push_back(static_cast<std::int32_t>(p));
    g_out.push_back(static_cast<std::int32_t>(q));
  }
  return static_cast<std::int32_t>(g_out.size() / 2);
}

// (id, remaining qty) pairs at one price, in queue order, into ob_out(). Returns the pair count.
EXPORT(ob_queue) std::int32_t ob_queue(std::int32_t side, std::int32_t price) {
  g_out.clear();
  for (OrderId id : book().queue_at(side_of(side), price)) {
    g_out.push_back(static_cast<std::int32_t>(id));
    g_out.push_back(static_cast<std::int32_t>(book().order_qty(id)));
  }
  return static_cast<std::int32_t>(g_out.size() / 2);
}

EXPORT(ob_out) const std::int32_t* ob_out() { return g_out.data(); }

// ---- benchmark exports ------------------------------------------------------

EXPORT(bench_generate) void bench_generate(std::int32_t n, std::int32_t seed) {
  generate(static_cast<std::size_t>(n), static_cast<std::uint64_t>(seed));
}
EXPORT(bench_avg_resting) double bench_avg_resting() { return g_avg_resting; }
EXPORT(bench_avg_levels) double bench_avg_levels() { return g_avg_levels; }

// which: 0 std::map levels, 1 tick array + bitset.
EXPORT(bench_replay) void bench_replay(std::int32_t which) {
  if (which == 0) replay<MapLevels>();
  else replay<ArrayLevels>();
}
EXPORT(bench_trades) double bench_trades() { return static_cast<double>(g_last.trades); }
EXPORT(bench_traded_qty) double bench_traded_qty() { return static_cast<double>(g_last.traded_qty); }
