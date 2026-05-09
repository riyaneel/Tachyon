import initTachyonWasm, {
  WasmBus,
} from "/extension/vendor/@tachyon-ipc/core/wasm/tachyon_ipc.js";

const CAPACITY = 1 << 20;
const MSG_SIZE = 16;
const PAGE_TO_SW_TYPE = (10 << 16) | 1;
const SW_TO_PAGE_TYPE = (10 << 16) | 2;
const WARMUP = 100;
const wasm = await initTachyonWasm();

const els = {
  status: document.querySelector("#status"),
  extensionId: document.querySelector("#extension-id"),
  connect: document.querySelector("#connect"),
  bench: document.querySelector("#bench"),
  iterations: document.querySelector("#iterations"),
  benchTable: document.querySelector("#bench-table"),
};

let nextId = 1;
let directPort;
let inFlightId = 0;
let inFlightStartedAt = 0;
let bench;
const pageToSw = new WasmBus(CAPACITY);
const swToPage = new WasmBus(CAPACITY);
const memoryWords = new Uint32Array(wasm.memory.buffer);
const requestMessage = { id: 0, value: 0 };

function sendThroughPageTachyon(id, value) {
  let offset = pageToSw.acquireTx(MSG_SIZE) >>> 2;
  memoryWords[offset] = id >>> 0;
  memoryWords[offset + 1] = value >>> 0;
  memoryWords[offset + 2] = PAGE_TO_SW_TYPE;
  memoryWords[offset + 3] = 0;
  pageToSw.commitTx(MSG_SIZE, PAGE_TO_SW_TYPE);

  if (!pageToSw.acquireRx()) throw new Error("page Tachyon request ring was empty");
  offset = pageToSw.rxPtr() >>> 2;
  requestMessage.id = memoryWords[offset];
  requestMessage.value = memoryWords[offset + 1];
  pageToSw.commitRx();
  return requestMessage;
}

function receiveThroughPageTachyon(message) {
  const offset = swToPage.acquireTx(MSG_SIZE) >>> 2;
  memoryWords[offset] = message.id >>> 0;
  memoryWords[offset + 1] = message.value >>> 0;
  memoryWords[offset + 2] = SW_TO_PAGE_TYPE;
  memoryWords[offset + 3] = 0;
  swToPage.commitTx(MSG_SIZE, SW_TO_PAGE_TYPE);

  if (!swToPage.acquireRx()) throw new Error("page Tachyon reply ring was empty");
  swToPage.commitRx();
}

function connectDirect() {
  const extensionId =
    els.extensionId.value.trim() ||
    new URLSearchParams(window.location.search).get("extensionId");
  if (!extensionId) throw new Error("extension id is required");
  if (!globalThis.chrome?.runtime?.connect) {
    throw new Error("chrome.runtime.connect is unavailable on this page");
  }

  directPort = chrome.runtime.connect(extensionId, { name: "tachyon-page" });
  directPort.onMessage.addListener(handleBridgeMessage);
  directPort.onDisconnect.addListener(() => {
    directPort = undefined;
    els.status.textContent = chrome.runtime.lastError?.message ?? "disconnected";
  });
  els.status.textContent = "direct external port";
}

function postPing(value) {
  if (!directPort) throw new Error("direct extension port is not connected");
  if (inFlightId) throw new Error("native request already in flight");
  inFlightId = nextId++;
  inFlightStartedAt = performance.now();
  directPort.postMessage(sendThroughPageTachyon(inFlightId, value));
}

function handleBridgeMessage(message) {
  if (inFlightId !== message.id) return;
  const totalUs = (performance.now() - inFlightStartedAt) * 1000;
  inFlightId = 0;
  if (message.op === "error") return failBench(new Error(message.message));
  receiveThroughPageTachyon(message);
  if (bench.warmup) {
    bench.warmup -= 1;
    if (!bench.warmup) bench.startedAt = performance.now();
  } else {
    bench.samples[bench.index++] = totalUs;
  }
  bench.index === bench.iterations ? finishBench() : postPing(bench.index);
}

function percentile(sorted, pct) {
  return sorted[Math.floor((sorted.length - 1) * pct)];
}

function setRows(rows) {
  els.benchTable.replaceChildren(
    ...rows.map(([label, value]) => {
      const row = document.createElement("tr");
      row.append(document.createElement("td"), document.createElement("td"));
      row.children[0].textContent = label;
      row.children[1].textContent = value;
      return row;
    }),
  );
}

function formatUs(us) {
  return us >= 1000 ? `${(us / 1000).toFixed(2)} ms` : `${us.toFixed(1)} us`;
}

function failBench(error) {
  bench = undefined;
  inFlightId = 0;
  els.status.textContent = error.message;
  els.bench.disabled = false;
}

function finishBench() {
  const { iterations, samples, startedAt } = bench;
  const totalMs = performance.now() - startedAt;
  bench = undefined;
  samples.sort();
  setRows([
    ["Path", "page -> service worker -> native host -> Tachyon"],
    ["Mode", "single in-flight, no local buffering"],
    ["Samples", `${iterations.toLocaleString()} page-observed RTTs`],
    ["p50", formatUs(percentile(samples, 0.5))],
    ["p90", formatUs(percentile(samples, 0.9))],
    ["p99", formatUs(percentile(samples, 0.99))],
    ["mean", formatUs((totalMs * 1000) / iterations)],
    ["throughput", `${(iterations / (totalMs / 1000)).toFixed(0)} RTT/sec`],
  ]);
  els.bench.disabled = false;
}

function runBench() {
  const iterations = Math.max(
    1,
    Number.parseInt(els.iterations.value, 10) || 10000,
  );
  els.bench.disabled = true;
  setRows([["Running", `${iterations.toLocaleString()} RTTs`]]);
  bench = {
    iterations,
    samples: new Float64Array(iterations),
    index: 0,
    warmup: WARMUP,
    startedAt: 0,
  };
  postPing(0);
}

els.connect.addEventListener("click", () => {
  try {
    connectDirect();
  } catch (error) {
    els.status.textContent = error.message;
  }
});
els.bench.addEventListener("click", () => {
  try {
    runBench();
  } catch (error) {
    failBench(error);
  }
});

const extensionId = new URLSearchParams(window.location.search).get("extensionId");
if (extensionId) els.extensionId.value = extensionId;
els.connect.click();
