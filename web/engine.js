// Thin JS wrapper over orderbook.wasm (the C++ engine compiled by build.sh).
// Works in a page, a Web Worker and Node.

export const SIDE = { buy: 0, sell: 1 };
export const TYPE = { limit: 0, market: 1, ioc: 2 };
export const REJECT = ["invalid id", "invalid qty", "invalid price", "duplicate id", "unknown id", "pool exhausted"];
const KIND = ["accepted", "rejected", "trade", "cancelled", "modified"];

export async function loadEngine(source = new URL("./orderbook.wasm", import.meta.url)) {
  let instance;
  if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
    ({ instance } = await WebAssembly.instantiate(source, {}));
  } else {
    const res = fetch(source);
    ({ instance } = WebAssembly.instantiateStreaming
      ? await WebAssembly.instantiateStreaming(res, {}).catch(async () => WebAssembly.instantiate(await (await fetch(source)).arrayBuffer(), {}))
      : await WebAssembly.instantiate(await (await res).arrayBuffer(), {}));
  }
  const x = instance.exports;
  x._initialize();
  const i32 = (ptr, n) => new Int32Array(x.memory.buffer, ptr, n);

  function events(n) {
    const raw = i32(x.ob_events(), n * 5);
    const out = [];
    for (let k = 0; k < n; k++) {
      const [kind, id, a, b, c] = raw.subarray(k * 5, k * 5 + 5);
      const type = KIND[kind];
      if (type === "trade") out.push({ type, maker: id, taker: a, price: b, qty: c });
      else if (type === "rejected") out.push({ type, id, reason: REJECT[a] });
      else if (type === "cancelled") out.push({ type, id, qty: a });
      else out.push({ type, id });
    }
    return out;
  }
  const pairs = (n) => {
    const raw = i32(x.ob_out(), n * 2);
    return Array.from({ length: n }, (_, k) => [raw[k * 2], raw[k * 2 + 1]]);
  };

  return {
    reset: () => x.ob_reset(),
    add: (id, side, type, price, qty) => events(x.ob_add(id, SIDE[side], TYPE[type], price, qty)),
    cancel: (id) => events(x.ob_cancel(id)),
    modify: (id, price, qty) => events(x.ob_modify(id, price, qty)),
    best: (side) => { const p = x.ob_best(SIDE[side]); return p < 0 ? null : p; },
    orderCount: () => x.ob_order_count(),
    orderQty: (id) => x.ob_order_qty(id),
    // { side, price, qty } of a resting order, or null
    order: (id) => {
      const s = x.ob_order_side(id);
      return s < 0 ? null : { side: s === 0 ? "buy" : "sell", price: x.ob_order_price(id), qty: x.ob_order_qty(id) };
    },
    depth: (side, maxLevels = 1e6) => pairs(x.ob_depth(SIDE[side], maxLevels)),
    queue: (side, price) => pairs(x.ob_queue(SIDE[side], price)).map(([id, qty]) => ({ id, qty })),
    bench: {
      generate: (n, seed) => x.bench_generate(n, seed),
      stats: () => ({ avgResting: x.bench_avg_resting(), avgLevels: x.bench_avg_levels() }),
      replay: (which) => { x.bench_replay(which === "map" ? 0 : 1); return { trades: x.bench_trades(), tradedQty: x.bench_traded_qty() }; },
    },
  };
}
