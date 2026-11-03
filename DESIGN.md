# Design notes

Why each choice was made, written to be talked through in an interview.

## 1. Matching rules

- **Price-time priority.** An incoming order trades against the best opposite price first and, within a price, against the oldest order first. Trades happen at the **maker's** (resting order's) price, so an aggressive buyer with a limit of 105 that hits an ask at 100 pays 100.
- **Market** orders ignore price and sweep until filled or the opposite side is empty; any remainder is cancelled, never rested. **IOC** orders do the same but stop at their limit price.
- **Validation happens before anything changes.** That includes checking pool capacity for limit orders, so an order is never partially filled and then rejected. The checks always run in the same order (id, qty, price, duplicate, capacity), which lets the reference book reproduce the same reject reasons.
- **Modify** follows common exchange behaviour:
  - Same price with quantity reduced (or unchanged): amend in place and **keep queue position**. Reducing your order shouldn't cost you priority.
  - New price, or quantity increased: **lose priority**. Otherwise you could hold your place in the queue and then enlarge the order. The engine removes the order and re-submits it as a new limit order with the same id, so a new price that crosses trades immediately.
- **Ids** are unique among resting orders only. Once an order fills or is cancelled, its id can be reused.

## 2. Orders at a price: intrusive doubly linked list

Every `Order` holds its own `prev` and `next` pointers plus a pointer to its `PriceLevel`.

- **Why a list at all:** time priority means FIFO, and a cancel can hit any position in the queue. With a `std::deque` or `std::vector` per level, a mid-queue cancel is O(n) and shifts elements.
- **Why intrusive:** the links live inside the order, so there's no separate node to allocate. Given an order pointer (from the id map), the book unlinks it in O(1).
- `PriceLevel` also keeps `total_qty` and `count` up to date, so volume queries are O(1).

## 3. Id lookup: open-addressing hash map (`FlatIdMap`)

- `std::unordered_map` allocates a node on every insert and chases a pointer on every lookup. `FlatIdMap` stores keys and values in two flat arrays and uses **linear probing**, so a lookup usually touches one or two adjacent cache lines.
- Its capacity is fixed at `2 × max_orders` (rounded up to a power of two, so a mask replaces modulo). The **load factor therefore never exceeds 0.5**, which keeps probe chains short and means insert can never fail while the pool has room.
- **Backward-shift deletion instead of tombstones.** The book deletes constantly: every cancel and every full fill. With tombstones, chains would fill with dead slots and lookups would slow down over time. Backward-shift deletion walks the cluster after the removed slot and moves an entry back into the hole if its home slot is cyclically at or before the hole. The test is `(j - home) & mask >= (j - hole) & mask`. That restores the invariant that every key is reachable from its home slot.
- Hashing uses the splitmix64 finalizer. Exchange ids are often sequential, and with an identity hash they would land in consecutive slots and form long clusters.
- Key 0 marks an empty slot, which is why order id 0 is rejected.

## 4. Order storage: object pool

All `Order` objects are allocated once. A free list (a `std::vector<Order*>` with reserved capacity) gives O(1) acquire and release with no `new` or `delete` while running. Resting order count is bounded by `max_orders`; when the pool is full, new limit orders are rejected with `PoolExhausted`. Market and IOC orders never rest, so they never need a slot.

## 5. Price levels: two implementations behind one interface

Both provide `get_or_create`, `remove`, `best`, `find` and `for_each_level`. `OrderBook<Levels, Listener>` contains all the matching logic, so the implementations can't drift apart in behaviour, only in speed.

**`MapLevels`** uses `std::map<Price, PriceLevel>`: a red-black tree with bids sorted descending and asks ascending.
- `best()` is `begin()`. Insert and erase are O(log L) for L live levels.
- It works for any price range, but every new price level is a heap allocation, and walking the tree means pointer-chasing.
- Map nodes never move, so a `PriceLevel*` stays valid for as long as the level exists.

**`ArrayLevels`** uses a flat array per side, indexed by `price - min_price`.
- `get_or_create` is an index plus setting one bit. The cached best index updates in O(1) when a better level appears.
- When the best level empties, the next best is found with an **occupancy bitset**: one bit per tick, scanned a 64-bit word at a time with `std::countl_zero` (bids) or `std::countr_zero` (asks). An empty gap of G ticks costs about G/64 word reads, and in practice the next level is usually a few ticks away, often in the same word.
- **Trade-off:** memory is proportional to the price *range*, not the number of live levels. 100k ticks × 40-byte levels × 2 sides is about 8 MB. That's acceptable for one instrument with a known tick band, but not for an unbounded range, which is why the map version exists.
- After construction it never allocates. `test_no_alloc.cpp` proves this by counting `operator new` calls across 300k operations.

## 6. Listener as a template parameter

Events are delivered through `L&`, where `L` satisfies the `Listener` concept. Because the type is known at compile time, the compiler can inline the callbacks: no vtable, no `std::function`. Tests use a recording listener, and the benchmark uses one that only counts. One limitation: callbacks run synchronously inside `add`, `cancel` and `modify`, so a listener must not call back into the book.

## 7. How correctness is established

1. **Unit tests** state each rule directly, and they run against both books (GoogleTest typed tests).
2. **Differential testing.** `ReferenceBook` is the simplest possible correct model: one vector, an O(n) scan for the best order on every step, and a sequence number for time priority. Random streams, including invalid inputs, go to all three books, and the event logs must be **identical after every operation**. This finds bugs that hand-written tests miss, especially around bitset word boundaries and modify. It was mutation-checked: deliberately injected bugs were caught.
3. **Sanitizers** (ASan + UBSan) catch memory errors and undefined behaviour in the pointer-heavy code.
4. **The benchmark cross-checks itself:** both books must report identical trade counts and traded volume on the same stream.

## 8. Benchmark methodology

- The stream is generated once, untimed, and replayed into a fresh book for each run. Every implementation sees identical input.
- Generated prices are anchored to the current touch, so passive orders don't cross and price only moves when aggressive orders consume levels. Cancels are throttled while the book is below 5,000 orders, which keeps the replay running against a realistic book instead of an almost empty one.
- Throughput is the median of several replays with no per-message timing. Latency comes from a separate pass that wraps every call in two `steady_clock::now()` calls; that clock costs about 29 ns per measurement, which is included in the reported percentiles.
- Numbers vary between runs and machines (the README reports ranges for two CPUs). On a fast desktop CPU the gap narrows, because with only ~63 live levels the `std::map` tree stays in cache; the array book’s advantage grows when the tree outgrows cache or the machine is slower.

## 9. What I'd do next

- Replace the `std::vector<Order*>` free list with an intrusive free list threaded through unused `Order` slots, saving one array.
- Use cache-line-aligned levels and hot/cold splitting of `Order` fields.
- Add a binary market-data parser (ITCH-style) to replay real feed files.
- Add stop, iceberg and post-only order types, and self-trade prevention.
- Support multiple instruments with one book per symbol on a single-writer thread, fed from a lock-free SPSC queue.
