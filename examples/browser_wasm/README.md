# Tachyon Browser WASM Example

This example runs Tachyon inside a single browser page. Page JavaScript writes
binary payloads directly into a `WasmBus` TX slot in WebAssembly memory, a small
Rust WASM function polls the inbound ring and replies on a second ring, and
JavaScript reads the reply from WASM memory.

The Rust echo function lives in `examples/browser_wasm/rust`; the reusable
browser transport stays in the Tachyon bindings.

The browser build does not use POSIX shared memory or UNIX sockets. Those APIs
are unavailable in browsers, so the WASM path is a page-local Tachyon ring with
the same 64-byte message header, alignment, `type_id`, and skip-marker rules.

## Run

```bash
cd examples/browser_wasm
npm install
npm run build:wasm
npm run dev
```

Open the Vite URL, then use **Send To Rust** or **Run Browser RTT Bench**.

## Native Comparison

From the same directory:

```bash
npm run bench:native
```

The browser benchmark reports batch-averaged round-trip time because
`performance.now()` is too coarse for individual sub-microsecond samples in many
browsers. Compare the browser mean/p50 against the native Rust `bench_ipc`
output for a practical JS/WASM overhead view.
