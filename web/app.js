import { loadEngine } from "./engine.js";

// ?embed hides the page chrome and reports the content height to the parent
// page, so an iframe can size itself without a scrollbar.
if (new URLSearchParams(location.search).has("embed")) {
  document.body.classList.add("embed");
  const report = () =>
    window.parent.postMessage({ type: "orderbook-demo-height", height: document.body.getBoundingClientRect().height }, "*");
  new ResizeObserver(report).observe(document.body);
  // The parent may not be listening yet when the first report goes out.
  for (const ms of [500, 1500, 3000, 6000]) setTimeout(report, ms);
}

const $ = (id) => document.getElementById(id);
const LEVELS = 10;      // rows shown per side
const CHIPS = 7;        // orders shown per row before "+N"
const MIN_PX = 9000, MAX_PX = 11000;
const fmt = (ticks) => (ticks / 100).toFixed(2);
const toTicks = (dollars) => Math.round(Number(dollars) * 100);

const state = {
  eng: null,
  nextId: 1,
  mine: new Set(),
  selected: 0,
  side: "buy",
  type: "limit",
  trades: 0,
  volume: 0,
  last: null,
  flash: new Set(),   // "side:price" rows to flash on next render
  flow: null,
  flowCount: 0,
};

// ---------------------------------------------------------------- engine I/O

function send(fn, label) {
  const events = fn();
  for (const e of events) {
    if (e.type === "trade") {
      state.trades++;
      state.volume += e.qty;
      state.last = e.price;
      const makerSide = state.eng.order(e.maker)?.side; // null if fully filled
      state.flash.add(`${makerSide || "any"}:${e.price}`);
    }
  }
  logEvents(events, label);
  return events;
}

function submit(side, type, price, qty, { mine = false } = {}) {
  const id = state.nextId++;
  if (mine) state.mine.add(id);
  const label = `${side} ${type} ${qty}${type === "market" ? "" : " @ " + fmt(price)}`;
  send(() => state.eng.add(id, side, type, type === "market" ? 0 : price, qty), `#${id} ${label}`);
  return id;
}

// ---------------------------------------------------------------- rendering

function renderSide(side, el) {
  const depth = state.eng.depth(side, LEVELS);
  const max = Math.max(1, ...depth.map(([, q]) => q), ...state.eng.depth(side === "buy" ? "sell" : "buy", LEVELS).map(([, q]) => q));
  const rows = [];
  for (let i = 0; i < LEVELS; i++) {
    const lvl = depth[i];
    if (!lvl) { rows.push(`<div class="lvl ${side === "buy" ? "bid" : "ask"} empty"><span class="px">—</span><span></span><span></span></div>`); continue; }
    const [price, qty] = lvl;
    const queue = state.eng.queue(side, price);
    const chips = queue.slice(0, CHIPS).map((o) =>
      `<span class="chip${state.mine.has(o.id) ? " mine" : ""}${o.id === state.selected ? " sel" : ""}" data-id="${o.id}" title="Order #${o.id}: ${o.qty}">#${o.id}·${o.qty}</span>`).join("");
    const more = queue.length > CHIPS ? `<span class="chip more">+${queue.length - CHIPS}</span>` : "";
    const flash = state.flash.has(`${side}:${price}`) || state.flash.has(`any:${price}`);
    rows.push(`<div class="lvl ${side === "buy" ? "bid" : "ask"}${flash ? " flash" : ""}">
      <div class="bar" style="width:${(qty / max) * 100}%"></div>
      <span class="px">${fmt(price)}</span><span>${qty}</span><div class="queue">${chips}${more}</div></div>`);
  }
  el.innerHTML = rows.join("");
}

function render() {
  const { eng } = state;
  renderSide("sell", $("asks"));
  renderSide("buy", $("bids"));
  state.flash.clear();
  const bid = eng.best("buy"), ask = eng.best("sell");
  $("spread").textContent =
    bid != null && ask != null ? `Spread ${fmt(ask - bid)} · mid ${((bid + ask) / 200).toFixed(3)}`
    : bid == null && ask == null ? "Empty book" : "One-sided book";
  $("touch").textContent = `${bid != null ? fmt(bid) : "—"} × ${ask != null ? fmt(ask) : "—"}`;
  const levels = eng.depth("buy").length + eng.depth("sell").length;
  $("stats").innerHTML = [
    ["Resting orders", eng.orderCount()],
    ["Price levels", levels],
    ["Last trade", state.last != null ? fmt(state.last) : "—"],
    ["Trades", state.trades.toLocaleString()],
    ["Volume", state.volume.toLocaleString()],
    ["Orders sent", (state.nextId - 1).toLocaleString()],
  ].map(([k, v]) => `<div><dt>${k}</dt><dd>${v}</dd></div>`).join("");
  renderSelected();
}

function renderSelected() {
  const o = state.selected ? state.eng.order(state.selected) : null;
  if (state.selected && !o) state.selected = 0;
  const on = !!o;
  if (!on) { $("m-price").value = ""; $("m-qty").value = ""; delete $("m-price").dataset.for; }
  for (const id of ["m-price", "m-qty", "modify", "cancel"]) $(id).disabled = !on;
  $("sel-label").textContent = on ? `#${state.selected} · ${o.side} ${o.qty} @ ${fmt(o.price)}${state.mine.has(state.selected) ? " · yours" : ""}` : "none (click an order in the book)";
  if (on && document.activeElement !== $("m-price") && document.activeElement !== $("m-qty") && $("m-price").dataset.for !== String(state.selected)) {
    $("m-price").value = fmt(o.price);
    $("m-qty").value = o.qty;
    $("m-price").dataset.for = String(state.selected);
  }
}

const tape = [];
function logEvents(events, label) {
  const batch = [];
  for (const e of events) {
    let k = e.type, text;
    if (e.type === "accepted") text = label;
    else if (e.type === "trade") text = `${e.qty} @ ${fmt(e.price)} · maker #${e.maker}, taker #${e.taker}`;
    else if (e.type === "cancelled") text = `#${e.id} · ${e.qty} cancelled`;
    else if (e.type === "rejected") text = `${label} · ${e.reason}`;
    else text = `#${e.id}`;
    batch.push({ k, text });
  }
  tape.unshift(...batch); // newest message on top, its events in engine order
  tape.length = Math.min(tape.length, 40);
  $("tape").innerHTML = tape.slice(0, 9).map((t, i) =>
    `<li class="${t.k}${i < events.length ? " new" : ""}"><span class="k">${t.k}</span><span>${t.text}</span></li>`).join("");
}

// ---------------------------------------------------------------- order form

function syncForm() {
  document.querySelectorAll("[data-side]").forEach((b) => b.classList.toggle("on", b.dataset.side === state.side));
  document.querySelectorAll("[data-type]").forEach((b) => b.classList.toggle("on", b.dataset.type === state.type));
  $("price").disabled = state.type === "market";
  const qty = Number($("qty").value) || 0;
  const btn = $("submit");
  btn.textContent = `${state.side === "buy" ? "Buy" : "Sell"} ${qty}${state.type === "market" ? " at market" : " @ " + Number($("price").value || 0).toFixed(2)}${state.type === "ioc" ? " IOC" : ""}`;
  btn.classList.toggle("buy", state.side === "buy");
  btn.classList.toggle("sell", state.side === "sell");
}

document.querySelectorAll("[data-side]").forEach((b) => b.addEventListener("click", () => { state.side = b.dataset.side; syncForm(); }));
document.querySelectorAll("[data-type]").forEach((b) => b.addEventListener("click", () => { state.type = b.dataset.type; syncForm(); }));
$("price").addEventListener("input", syncForm);
$("qty").addEventListener("input", syncForm);
$("order").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const id = submit(state.side, state.type, toTicks($("price").value), Math.floor(Number($("qty").value)), { mine: true });
  if (state.eng.order(id)) state.selected = id;
  render();
});

$("asks").addEventListener("click", pick);
$("bids").addEventListener("click", pick);
function pick(ev) {
  const chip = ev.target.closest(".chip[data-id]");
  if (!chip) return;
  state.selected = Number(chip.dataset.id);
  delete $("m-price").dataset.for;
  render();
}
$("modify").addEventListener("click", () => {
  const id = state.selected;
  send(() => state.eng.modify(id, toTicks($("m-price").value), Math.floor(Number($("m-qty").value))), `#${id} modify`);
  delete $("m-price").dataset.for;
  render();
});
$("cancel").addEventListener("click", () => {
  send(() => state.eng.cancel(state.selected), `#${state.selected} cancel`);
  render();
});

// ---------------------------------------------------------------- simulated flow
// Roughly the benchmark's mix: passive limits a few ticks behind the touch,
// some marketable flow, cancels and modifies of random resting orders.

const rand = (n) => Math.floor(Math.random() * n);
const geo = () => { let k = 0; while (Math.random() > 0.35 && k < 12) k++; return k; };
const live = [];

function pickLive() {
  while (live.length) {
    const i = rand(live.length);
    const id = live[i];
    if (state.eng.order(id) && !state.mine.has(id)) return id;
    live[i] = live[live.length - 1];
    live.pop();
  }
  return 0;
}

function flowStep() {
  const { eng } = state;
  const bid = eng.best("buy"), ask = eng.best("sell");
  const mid = bid != null && ask != null ? (bid + ask) / 2 : bid ?? ask ?? 10000;
  const side = Math.random() < 0.5 ? "buy" : "sell";
  const sign = side === "buy" ? -1 : 1;
  const passive = side === "buy" ? (ask ?? Math.round(mid) + 1) - 1 : (bid ?? Math.round(mid) - 1) + 1;
  const far = side === "buy" ? (ask ?? Math.round(mid)) : (bid ?? Math.round(mid));
  const qty = 1 + rand(100);
  const clamp = (p) => Math.min(MAX_PX, Math.max(MIN_PX, p));
  let roll = rand(100);
  if (roll >= 60 && eng.orderCount() < 40) roll = 0;
  if (roll < 52) { const id = submit(side, "limit", clamp(passive + sign * geo()), qty); live.push(id); }
  else if (roll < 56) { const id = submit(side, "limit", clamp(far - sign * geo()), qty); live.push(id); }
  else if (roll < 58) submit(side, "ioc", clamp(far - sign * geo()), qty);
  else if (roll < 59) submit(side, "market", 0, 1 + rand(60));
  else if (roll < 92) { const id = pickLive(); if (id) send(() => eng.cancel(id), `#${id} cancel`); }
  else {
    const id = pickLive();
    const o = id && eng.order(id);
    if (o) {
      const reduce = Math.random() < 0.5 && o.qty > 1;
      const px = reduce ? o.price : clamp((o.side === "buy" ? (ask ?? mid + 1) - 1 - geo() : (bid ?? mid - 1) + 1 + geo()));
      send(() => eng.modify(id, Math.round(px), reduce ? Math.floor(o.qty / 2) : qty), `#${id} modify`);
    }
  }
}

function setFlow(on) {
  clearInterval(state.flow);
  state.flow = null;
  if (on) {
    const rate = Number($("speed").value);
    state.flow = setInterval(() => { flowStep(); render(); }, 1000 / rate);
    $("flow-rate").textContent = `${rate} msgs/s`;
  } else $("flow-rate").textContent = "";
  $("flow").textContent = on ? "❚❚ Pause flow" : "▶ Run flow";
}
$("flow").addEventListener("click", () => setFlow(!state.flow));
$("speed").addEventListener("input", () => { if (state.flow) setFlow(true); });

function clearBook() {
  state.eng.reset();
  Object.assign(state, { nextId: 1, selected: 0, trades: 0, volume: 0, last: null });
  state.mine.clear();
  live.length = 0;
  tape.length = 0;
  $("tape").innerHTML = "";
}

function seedBook() {
  clearBook();
  for (let lvl = 0; lvl < 9; lvl++) {
    for (const side of ["buy", "sell"]) {
      const n = 1 + rand(4);
      for (let k = 0; k < n; k++) {
        const price = side === "buy" ? 9999 - lvl - (lvl > 5 ? rand(3) : 0) : 10001 + lvl + (lvl > 5 ? rand(3) : 0);
        live.push(submit(side, "limit", price, 10 * (1 + rand(15))));
      }
    }
  }
  tape.length = 0;
  $("tape").innerHTML = '<li class="muted">Send an order or run the flow to see events.</li>';
}
$("seed").addEventListener("click", () => { seedBook(); render(); });
$("clear").addEventListener("click", () => { setFlow(false); clearBook(); render(); });

// ---------------------------------------------------------------- benchmark

let worker;
$("bench-run").addEventListener("click", () => {
  worker ??= new Worker(new URL("./bench-worker.js", import.meta.url), { type: "module" });
  const n = Number($("bench-n").value);
  $("bench-run").disabled = true;
  $("bars").innerHTML = "";
  worker.onmessage = ({ data }) => {
    if (data.type === "progress") $("bench-status").textContent = data.text;
    else if (data.type === "error") { $("bench-status").textContent = `Failed: ${data.text}`; $("bench-run").disabled = false; }
    else if (data.type === "done") { showBench(data.result); $("bench-run").disabled = false; }
  };
  worker.postMessage({ n });
});

function showBench({ n, stats, books: { map, array } }) {
  const max = Math.max(map.rate, array.rate);
  const row = (cls, name, b) => `<div class="brow ${cls}"><span>${name}</span>
    <div class="btrack"><div class="bfill" data-w="${(b.rate / max) * 100}"></div></div>
    <span class="val">${(b.rate / 1e6).toFixed(1)}M msgs/s</span></div>`;
  const same = map.trades === array.trades && map.tradedQty === array.tradedQty;
  $("bars").innerHTML = row("map", "std::map levels", map) + row("array", "tick array + bitset", array) +
    `<p class="bsum">Tick array is <strong>${(array.rate / map.rate).toFixed(2)}×</strong> faster here · ${n.toLocaleString()} messages,
     ~${Math.round(stats.avgResting).toLocaleString()} resting orders across ~${Math.round(stats.avgLevels)} levels ·
     ${same ? `both books: ${array.trades.toLocaleString()} trades, ${array.tradedQty.toLocaleString()} shares ✓` : "books disagree ✗"}</p>`;
  requestAnimationFrame(() => document.querySelectorAll(".bfill").forEach((f) => (f.style.width = f.dataset.w + "%")));
  $("bench-status").textContent = "Median of 3 replays per book.";
}

// ---------------------------------------------------------------- start

loadEngine().then((eng) => {
  state.eng = eng;
  seedBook();
  render();
  syncForm();
  $("submit").disabled = false;
  $("bench-run").disabled = false;
  const pill = $("engine");
  pill.dataset.state = "ready";
  pill.textContent = "Engine ready · WebAssembly";
}).catch((err) => {
  const pill = $("engine");
  pill.dataset.state = "error";
  pill.textContent = "Engine failed to load";
  console.error(err);
});
