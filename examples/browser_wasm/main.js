import init, {
  WasmBus,
  makeTypeId,
  msgType,
  routeId,
  tachyon_browser_echo_once,
} from "./pkg/tachyon_browser_wasm_example.js";

const CAPACITY = 1 << 20;
const BATCH_SIZE = 4096;

const els = {
  status: document.querySelector("#wasm-status"),
  capacity: document.querySelector("#capacity"),
  lastReply: document.querySelector("#last-reply"),
  value: document.querySelector("#value"),
  send: document.querySelector("#send"),
  iterations: document.querySelector("#iterations"),
  bench: document.querySelector("#bench"),
  log: document.querySelector("#log"),
  benchTable: document.querySelector("#bench-table"),
};

let wasm;
let jsToRust;
let rustToJs;
let typeCounter;

function memory() {
  return wasm.memory;
}

function appendLog(line) {
  const time = new Date().toLocaleTimeString();
  els.log.textContent = `[${time}] ${line}\n${els.log.textContent}`;
}

function writeU32ToBus(bus, value, typeId) {
  const ptr = bus.acquireTx(4);
  new DataView(memory().buffer, ptr, 4).setUint32(0, value >>> 0, true);
  bus.commitTx(4, typeId);
}

function readU32FromBus(bus) {
  if (!bus.acquireRx()) return null;
  const ptr = bus.rxPtr();
  const size = bus.rxSize();
  const typeId = bus.rxTypeId();
  const value =
    size === 4
      ? new DataView(memory().buffer, ptr, 4).getUint32(0, true)
      : null;
  bus.commitRx();
  return { value, size, typeId };
}

function pingRust(value) {
  writeU32ToBus(jsToRust, value, typeCounter);

  if (!tachyon_browser_echo_once(jsToRust, rustToJs)) {
    throw new Error("Rust WASM program did not receive the JS message");
  }

  const reply = readU32FromBus(rustToJs);
  if (!reply) {
    throw new Error("JS did not receive the Rust WASM reply");
  }

  return reply;
}

function pingRustFast(value) {
  writeU32ToBus(jsToRust, value, typeCounter);
  if (!tachyon_browser_echo_once(jsToRust, rustToJs)) {
    throw new Error("Rust WASM program did not receive the JS message");
  }
  const reply = readU32FromBus(rustToJs);
  if (!reply) {
    throw new Error("JS did not receive the Rust WASM reply");
  }
  return reply.value;
}

function percentile(sorted, pct) {
  const idx = Math.min(
    sorted.length - 1,
    Math.floor((sorted.length - 1) * pct),
  );
  return sorted[idx];
}

function formatNs(ns) {
  if (ns >= 1000) return `${(ns / 1000).toFixed(2)} us`;
  return `${ns.toFixed(1)} ns`;
}

function setBenchRows(rows) {
  els.benchTable.replaceChildren(
    ...rows.map(([label, value]) => {
      const tr = document.createElement("tr");
      const left = document.createElement("td");
      const right = document.createElement("td");
      left.textContent = label;
      right.textContent = value;
      tr.append(left, right);
      return tr;
    }),
  );
}

async function runBench() {
  const iterations = Math.max(
    1000,
    Number.parseInt(els.iterations.value, 10) || 1000000,
  );
  const warmup = Math.min(10000, Math.floor(iterations / 10));

  els.bench.disabled = true;
  setBenchRows([["Running", `${iterations.toLocaleString()} RTTs`]]);
  await new Promise((resolve) => requestAnimationFrame(resolve));

  for (let i = 0; i < warmup; i += 1) {
    pingRust(i);
  }

  const samples = [];
  let totalStart = performance.now();
  for (let i = 0; i < iterations; i += BATCH_SIZE) {
    const batchCount = Math.min(BATCH_SIZE, iterations - i);
    const batchStart = performance.now();
    for (let j = 0; j < batchCount; j += 1) {
      pingRustFast(i + j);
    }
    samples.push(((performance.now() - batchStart) * 1_000_000) / batchCount);
  }
  const totalMs = performance.now() - totalStart;

  samples.sort((a, b) => a - b);
  const throughput = iterations / (totalMs / 1000);
  setBenchRows([
    ["Payload", "4 bytes u32"],
    [
      "Samples",
      `${samples.length.toLocaleString()} batch averages x ${BATCH_SIZE}`,
    ],
    ["Direct doorbell p50", formatNs(percentile(samples, 0.5))],
    ["Direct doorbell p90", formatNs(percentile(samples, 0.9))],
    ["Direct doorbell p99", formatNs(percentile(samples, 0.99))],
    ["Direct doorbell mean", formatNs((totalMs * 1_000_000) / iterations)],
    ["Throughput", `${(throughput / 1000).toFixed(1)} K RTT/sec`],
  ]);
  appendLog(
    `browser bench completed: ${(throughput / 1000).toFixed(1)} K RTT/sec`,
  );
  els.bench.disabled = false;
}

async function main() {
  wasm = await init();
  typeCounter = makeTypeId(0, 7);
  jsToRust = new WasmBus(CAPACITY);
  rustToJs = new WasmBus(CAPACITY);

  els.status.textContent = "ready";
  els.capacity.textContent = `${CAPACITY / 1024} KiB x 2`;
  els.send.disabled = false;
  els.bench.disabled = false;

  els.send.addEventListener("click", () => {
    const value = Number.parseInt(els.value.value, 10) >>> 0;
    const reply = pingRust(value);
    els.lastReply.textContent = `${reply.value}`;
    appendLog(
      `JS sent ${value}, Rust replied ${reply.value}; route=${routeId(reply.typeId)} type=${msgType(
        reply.typeId,
      )}`,
    );
  });

  els.bench.addEventListener("click", () => {
    runBench().catch((err) => {
      appendLog(`bench failed: ${err.message}`);
      els.bench.disabled = false;
    });
  });

  appendLog("WASM module initialized");
}

main().catch((err) => {
  els.status.textContent = "failed";
  appendLog(err.stack || err.message);
});
