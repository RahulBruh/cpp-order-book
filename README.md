# C++ Limit Order Book & Matching Engine

A single-instrument limit order book with **price-time priority** matching, written in C++20. It has two interchangeable price-level stores: a `std::map` baseline and a tick-indexed array with an occupancy bitset. A randomized differential test checks both against a deliberately naive reference book, and a benchmark replays a seeded order-flow stream through each.

## Features

- **Order types:** limit (good-till-cancel), market and IOC. Supports partial fills, cancel, and modify.
- **Modify semantics:** a quantity reduction at the same price keeps the order's queue position. A price change or a quantity increase sends it to the back of the queue, and a new price can trade immediately.
- **Event stream:** every operation reports `accepted`, `rejected` (with a reason), `trade`, `cancelled` and `modified` events through a listener. The listener is a template parameter, so the calls inline and there's no virtual dispatch.
- **No heap allocation on the hot path** (tick-array book): orders come from a preallocated pool, and ids are looked up in a fixed-size open-addressing hash map. A test that counts `operator new` calls checks this.

## Data structures

| Piece | Structure | Cost |
|---|---|---|
| Orders at one price | Intrusive doubly linked FIFO (`Order::prev/next`) | O(1) append, O(1) cancel from anywhere |
| Id → order | Open-addressing hash map, linear probing, backward-shift deletion | O(1) expected; load ≤ 0.5 |
| Order storage | Fixed-capacity object pool | O(1) acquire/release, no allocation |
| Price levels, baseline | Two `std::map`s (bids descending, asks ascending) | O(log L) insert/erase, allocates per new level |
| Price levels, fast | Array indexed by tick + 64-bit occupancy bitset | O(1) insert; finding the next best level scans 64 ticks per word |

The matching logic lives once in `OrderBook<Levels, Listener>`, so the two books differ only in level storage. [DESIGN.md](DESIGN.md) explains each choice and its trade-offs.

## Correctness

- **Unit tests (GoogleTest):** 23 behavioural tests run against both books. They cover price and time priority, partial fills on both sides, market/IOC remainders, cancel from the middle of a queue, the three modify cases, every reject reason, pool exhaustion, range boundaries and multi-word bitset scans.
- **Differential test:** about 1.15M random operations across 5 scenarios (tight and wide spreads, a 24-order pool, bitset word edges). The same stream goes to the map book, the array book and `ReferenceBook`, which scans a flat vector for every operation. The **event streams must match exactly after every operation**, and full depth, order count and best prices must match every 64 operations. I checked that the test catches bugs by deliberately breaking things: an off-by-one in the bitset scan and a wrong modify-priority rule were both caught within the first ~7,000 operations.
- **Hash map test:** 400k random inserts and erases against `std::unordered_map`.
- **No-allocation test:** 300k random operations on the array book with zero calls to `operator new`. A control case confirms the map book does allocate.
- **Sanitizers:** the full suite also runs under AddressSanitizer + UBSan.

## Benchmark

`lob_bench` generates 5M messages (seed 42) and replays them into a fresh book for each run.

- **Message mix:** 53% add, 39% cancel, 8% modify. About 7% of messages are marketable.
- **Book size:** about **5,000 resting orders across ~63 price levels** during the replay.
- **Prices:** anchored to the current touch, so passive orders never cross.

Throughput is the median of 7 replays. Latency comes from a separate pass that times each message.

**AMD Ryzen 5 7600** (2-vCPU Linux VM), GCC 11.4 `-O3`:

| Book | Throughput (M msgs/s) | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|
| `std::map` levels | 49.67 | 40 | 100 | 180 | 380 |
| tick array + bitset | 62.02 | 40 | 110 | 250 | 400 |

Across four runs, the tick-array book handled **58–63M msgs/s** against 45–50M for `std::map`, a **1.22–1.33× speedup**. On this machine the timer overhead is about 20 ns per measurement, and latencies are measured in 10 ns steps, so per-message percentiles are close to what the timer can resolve and don't separate the two books reliably. Throughput is the reliable comparison.

**Intel Xeon @ 2.10 GHz** (2-vCPU shared cloud VM), GCC 13.3 `-O3`:

| Book | Throughput (M msgs/s) | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|
| `std::map` levels | 20.45 | 103 | 173 | 355 | 587 |
| tick array + bitset | 32.63 | 90 | 154 | 315 | 546 |

On the slower Xeon, the array book's advantage was bigger: **1.60–1.96×** across four runs. Its larger caches likely help the Ryzen keep the small (~63-level) `std::map` tree hot. Latencies include the `steady_clock` overhead (about 29 ns on this machine). Both books must report identical trade counts and traded volume, or the benchmark exits with an error.

## Build

Requires CMake ≥ 3.20 and a C++20 compiler. GoogleTest comes from the system if installed; otherwise CMake downloads a pinned release.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lob_bench            # --messages N --seed S --reps R

# sanitizers
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOB_SANITIZE=ON
cmake --build build-asan -j && ctest --test-dir build-asan
```

CI (GitHub Actions) builds with GCC and Clang with warnings as errors, runs the tests, runs them again under ASan/UBSan, and runs a short benchmark.

## Layout

```
include/lob/   order_book.hpp (matching), array_levels.hpp, map_levels.hpp,
               price_level.hpp (intrusive FIFO), flat_id_map.hpp, object_pool.hpp, types.hpp
tests/         unit, differential, hash-map and no-allocation tests; reference_book.hpp
bench/         bench.cpp
```

## Not in scope

This book handles one instrument on one thread. It has no networking, no market-data protocol parsing, no self-trade prevention, and no stop or iceberg orders.
