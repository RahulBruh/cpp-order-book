// Checks the WebAssembly build against the engine's documented behaviour and
// against the native benchmark built with libc++ (the random distributions differ
// between libc++ and libstdc++, so a GCC build generates a different stream).
//   node web/test_wasm.mjs [native trades] [native shares]
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";
import { loadEngine } from "./engine.js";

const eng = await loadEngine(readFileSync(new URL("./orderbook.wasm", import.meta.url)));
let checks = 0;
const eq = (a, b, msg) => { assert.deepEqual(a, b, msg); checks++; };

// Price-time priority: two sells at 100.00, then one at 100.01; a buy for 150 at 100.01
eng.reset();
eng.add(1, "sell", "limit", 10000, 100);
eng.add(2, "sell", "limit", 10000, 50);
eng.add(3, "sell", "limit", 10001, 70);
let ev = eng.add(4, "buy", "limit", 10001, 200);
eq(ev.filter((e) => e.type === "trade").map((t) => [t.maker, t.price, t.qty]), [[1, 10000, 100], [2, 10000, 50], [3, 10001, 50]], "fills oldest first, best price first");
eq(eng.depth("sell"), [[10001, 20]], "remaining ask");
eq(eng.best("bid"), null, "taker fully filled, nothing rests");
eq(eng.queue("sell", 10001), [{ id: 3, qty: 20 }], "partially filled maker keeps the rest");

// Remainders: a limit rests, IOC and market cancel
eng.reset();
eng.add(1, "sell", "limit", 10010, 30);
ev = eng.add(2, "buy", "ioc", 10010, 50);
eq(ev.map((e) => e.type), ["accepted", "trade", "cancelled"], "IOC cancels its remainder");
eq(ev[2].qty, 20, "IOC remainder qty");
eng.add(3, "sell", "limit", 10020, 10);
ev = eng.add(4, "buy", "market", 0, 25);
eq(ev.map((e) => e.type), ["accepted", "trade", "cancelled"], "market cancels its remainder");
eng.add(5, "buy", "limit", 9990, 40);
eq(eng.best("buy"), 9990, "limit rests");

// Modify: same price, smaller qty keeps queue position; a new price loses it
eng.reset();
eng.add(1, "buy", "limit", 10000, 10);
eng.add(2, "buy", "limit", 10000, 10);
eng.modify(1, 10000, 5);
eq(eng.queue("buy", 10000).map((o) => o.id), [1, 2], "reduce keeps priority");
eng.modify(1, 10000, 8);
eq(eng.queue("buy", 10000).map((o) => o.id), [2, 1], "increase loses priority");

// Rejects
eng.reset();
eq(eng.add(1, "buy", "limit", 50, 10)[0].reason, "invalid price", "outside tick range");
eq(eng.add(0, "buy", "limit", 10000, 10)[0].reason, "invalid id", "id 0");
eq(eng.cancel(99)[0].reason, "unknown id", "cancel unknown");
eng.add(7, "buy", "limit", 10000, 10);
eq(eng.add(7, "buy", "limit", 10000, 10)[0].reason, "duplicate id", "duplicate");

// Benchmark: both books agree, and match the native lob_bench for the same stream
eng.bench.generate(1_000_000, 42);
const map = eng.bench.replay("map");
const arr = eng.bench.replay("array");
eq(map, arr, "map and array books trade identically");
const [nTrades, nShares] = process.argv.slice(2).map(Number);
if (nTrades) eq([arr.trades, arr.tradedQty], [nTrades, nShares], "matches native lob_bench");
console.log(`ok: ${checks} checks; 1M messages -> ${arr.trades} trades, ${arr.tradedQty} shares`, eng.bench.stats());
