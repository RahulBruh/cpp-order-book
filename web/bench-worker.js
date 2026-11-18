// Runs the benchmark off the main thread so the page stays responsive.
import { loadEngine } from "./engine.js";

const ready = loadEngine();
const REPS = 3;

self.onmessage = async ({ data: { n, seed = 42 } }) => {
  try {
    const eng = await ready;
    self.postMessage({ type: "progress", text: `Generating ${n.toLocaleString()} messages…` });
    eng.bench.generate(n, seed);
    const stats = eng.bench.stats();
    const result = { n, stats, books: {} };
    for (const which of ["map", "array"]) {
      const rates = [];
      let trades;
      for (let r = 0; r < REPS; r++) {
        self.postMessage({ type: "progress", text: `Replaying into ${which === "map" ? "std::map" : "tick-array"} book (${r + 1}/${REPS})…` });
        const t0 = performance.now();
        trades = eng.bench.replay(which);
        const ms = performance.now() - t0;
        rates.push(n / (ms / 1000));
      }
      rates.sort((a, b) => a - b);
      result.books[which] = { rate: rates[Math.floor(rates.length / 2)], ...trades };
    }
    self.postMessage({ type: "done", result });
  } catch (err) {
    self.postMessage({ type: "error", text: String(err && err.message || err) });
  }
};
