// Replays a seeded, synthetic order-flow stream through each book
// implementation and reports throughput and per-message latency.
//
//   lob_bench [--messages N] [--seed S] [--reps R]
//
// The stream is generated once (untimed) and replayed into a fresh book for
// every run, so every implementation sees exactly the same messages. Both
// books must report the same traded volume, which doubles as a sanity check.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

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
  std::uint64_t traded_qty = 0, trades = 0, accepted = 0, cancelled = 0, rejected = 0;
  void on_accepted(OrderId) { ++accepted; }
  void on_rejected(OrderId, RejectReason) { ++rejected; }
  void on_trade(const Trade& t) {
    ++trades;
    traded_qty += static_cast<std::uint64_t>(t.qty);
  }
  void on_cancelled(OrderId, Qty) { ++cancelled; }
  void on_modified(OrderId) {}
};

constexpr Config kCfg{.min_price = 0, .max_price = 99'999, .max_orders = 1 << 18};

// Order flow loosely shaped like a real book: most adds are passive and land
// a few ticks behind the touch (geometric distance), cancels target random
// live orders, ~7% of messages are marketable. Prices are anchored to the
// current touch, so passive orders never cross and the price moves only when
// aggressive flow eats through a level (as in a real market).
// Cancels are throttled while the book is below `target_resting` orders, so
// the replay runs against a book of realistic size rather than a near-empty one.
constexpr std::size_t kTargetResting = 5'000;

struct StreamStats {
  double avg_resting = 0;
  double avg_levels = 0;
};

std::vector<Msg> generate(std::size_t n, std::uint64_t seed, StreamStats& stats) {
  std::mt19937_64 rng(seed);
  std::geometric_distribution<int> depth(0.2);  // mean 4 ticks behind the touch
  std::uniform_int_distribution<int> pct(0, 99);
  std::uniform_int_distribution<Qty> size(1, 100);

  // A book is used to know which orders are still live when picking cancel
  // and modify targets; the generator's own state is not part of the timing.
  SinkListener sink;
  ArrayOrderBook<SinkListener> book(kCfg, sink);
  std::vector<OrderId> live;
  Price mid = 50'000;
  OrderId next_id = 1;

  auto pick_live = [&]() -> OrderId {
    while (!live.empty()) {
      std::size_t i = rng() % live.size();
      OrderId id = live[i];
      if (book.contains(id)) return id;
      live[i] = live.back();  // filled since we saw it; drop lazily
      live.pop_back();
    }
    return 0;
  };

  std::vector<Msg> msgs;
  msgs.reserve(n);
  while (msgs.size() < n) {
    // Touch for this message: the opposite side's best price, falling back to
    // the last known mid when that side is empty.
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask) mid = (*bid + *ask) / 2;
    const Side side = rng() % 2 ? Side::Buy : Side::Sell;
    const int sign = side == Side::Buy ? -1 : 1;
    // passive anchor: just inside our side of the spread; aggressive anchor: the far touch
    const Price passive = side == Side::Buy ? (ask ? *ask : mid + 1) - 1 : (bid ? *bid : mid - 1) + 1;
    const Price far_touch = side == Side::Buy ? (ask ? *ask : mid) : (bid ? *bid : mid);
    int roll = pct(rng);
    if (roll >= 52 && roll < 92 && book.order_count() < kTargetResting) roll = 0;  // add instead of cancel
    Msg m{};

    if (roll < 45) {  // passive limit
      m = {Op::Add, side, OrderType::Limit, next_id++, passive + sign * depth(rng), size(rng)};
      live.push_back(m.id);
    } else if (roll < 49) {  // marketable limit (rests any remainder)
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
      const Order* o = book.find_order(id);
      const Qty cur = o->qty;
      // half are in-place size reductions, half are passive reprices
      const Side oside = o->side;
      const bool reduce = rng() % 2 && cur > 1;
      const Price opassive = oside == Side::Buy ? (ask ? *ask : mid + 1) - 1 : (bid ? *bid : mid - 1) + 1;
      Price px = reduce ? o->price : opassive + (oside == Side::Buy ? -1 : 1) * depth(rng);
      Qty q = reduce ? cur / 2 : size(rng);
      m = {Op::Modify, oside, OrderType::Limit, id, px, q};
    }

    switch (m.op) {
      case Op::Add: book.add(m.id, m.side, m.type, m.price, m.qty); break;
      case Op::Cancel: book.cancel(m.id); break;
      case Op::Modify: book.modify(m.id, m.price, m.qty); break;
    }
    msgs.push_back(m);
    if (msgs.size() % 1024 == 0) {
      stats.avg_resting += static_cast<double>(book.order_count());
      stats.avg_levels += static_cast<double>(book.depth(Side::Buy).size() + book.depth(Side::Sell).size());
    }
  }
  const double samples = static_cast<double>(msgs.size() / 1024);
  stats.avg_resting /= samples;
  stats.avg_levels /= samples;
  return msgs;
}

template <class Book>
inline void apply(Book& book, const Msg& m) {
  switch (m.op) {
    case Op::Add: book.add(m.id, m.side, m.type, m.price, m.qty); break;
    case Op::Cancel: book.cancel(m.id); break;
    case Op::Modify: book.modify(m.id, m.price, m.qty); break;
  }
}

struct Result {
  double msgs_per_sec = 0;
  double p50 = 0, p90 = 0, p99 = 0, p999 = 0;
  std::uint64_t traded_qty = 0, trades = 0;
  std::size_t resting_at_end = 0;
};

template <class Levels>
Result run(const std::vector<Msg>& msgs, int reps) {
  Result r;
  std::vector<double> rates;
  for (int rep = 0; rep < reps; ++rep) {
    SinkListener sink;
    OrderBook<Levels, SinkListener> book(kCfg, sink);
    const auto t0 = Clock::now();
    for (const Msg& m : msgs) apply(book, m);
    const auto t1 = Clock::now();
    rates.push_back(static_cast<double>(msgs.size()) / std::chrono::duration<double>(t1 - t0).count());
    r.traded_qty = sink.traded_qty;
    r.trades = sink.trades;
    r.resting_at_end = book.order_count();
  }
  std::sort(rates.begin(), rates.end());
  r.msgs_per_sec = rates[rates.size() / 2];  // median rep

  // Separate pass for per-message latency (timing each call adds overhead,
  // so it is kept out of the throughput number).
  std::vector<std::uint32_t> ns(msgs.size());
  SinkListener sink;
  OrderBook<Levels, SinkListener> book(kCfg, sink);
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    const auto t0 = Clock::now();
    apply(book, msgs[i]);
    const auto t1 = Clock::now();
    ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }
  auto pct = [&](double p) {
    std::size_t k = std::min(ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(ns.size())));
    std::nth_element(ns.begin(), ns.begin() + static_cast<std::ptrdiff_t>(k), ns.end());
    return static_cast<double>(ns[k]);
  };
  r.p50 = pct(0.50);
  r.p90 = pct(0.90);
  r.p99 = pct(0.99);
  r.p999 = pct(0.999);
  return r;
}

double timer_overhead_ns() {
  std::vector<std::uint32_t> v(1'000'000);
  for (auto& x : v) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    x = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
  }
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t n = 5'000'000;
  std::uint64_t seed = 42;
  int reps = 5;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--messages")) n = std::strtoull(argv[i + 1], nullptr, 10);
    else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 10);
    else if (!std::strcmp(argv[i], "--reps")) reps = std::atoi(argv[i + 1]);
  }

  std::printf("Generating %zu messages (seed %llu)...\n", n, static_cast<unsigned long long>(seed));
  StreamStats stats;
  const auto msgs = generate(n, seed, stats);
  std::size_t adds = 0, cancels = 0, modifies = 0;
  for (const auto& m : msgs) (m.op == Op::Add ? adds : m.op == Op::Cancel ? cancels : modifies)++;
  const double total = static_cast<double>(n);
  std::printf("Mix: %.1f%% add, %.1f%% cancel, %.1f%% modify\n", 100.0 * static_cast<double>(adds) / total,
              100.0 * static_cast<double>(cancels) / total, 100.0 * static_cast<double>(modifies) / total);

  const Result map_r = run<MapLevels>(msgs, reps);
  const Result arr_r = run<ArrayLevels>(msgs, reps);

  if (map_r.traded_qty != arr_r.traded_qty || map_r.trades != arr_r.trades) {
    std::fprintf(stderr, "MISMATCH: books disagree on traded volume\n");
    return 1;
  }
  std::printf("Book during replay: %.0f resting orders across %.0f price levels on average\n", stats.avg_resting,
              stats.avg_levels);
  std::printf("Trades: %llu (%llu shares), resting at end: %zu\n", static_cast<unsigned long long>(arr_r.trades),
              static_cast<unsigned long long>(arr_r.traded_qty), arr_r.resting_at_end);
  std::printf("Timer overhead (median now()-now()): %.0f ns, included in latencies below\n\n", timer_overhead_ns());

  std::printf("| Book | Throughput (M msgs/s) | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) |\n");
  std::printf("|---|---:|---:|---:|---:|---:|\n");
  auto row = [](const char* name, const Result& r) {
    std::printf("| %s | %.2f | %.0f | %.0f | %.0f | %.0f |\n", name, r.msgs_per_sec / 1e6, r.p50, r.p90, r.p99, r.p999);
  };
  row("std::map levels", map_r);
  row("tick array + bitset", arr_r);
  std::printf("\nSpeedup (throughput): %.2fx\n", arr_r.msgs_per_sec / map_r.msgs_per_sec);
  return 0;
}
