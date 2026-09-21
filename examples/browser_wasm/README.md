# Tachyon Browser WASM Example

This example runs Tachyon inside a single browser page. Page JavaScript writes
binary payloads through the package Bus API into WebAssembly memory, a small
C++ WASM function (`tachyon_browser_echo_once`) polls the inbound ring and
replies on a second ring, and JavaScript reads the reply from WASM memory.

The demo program lives in `examples/browser_wasm/echo/echo.cpp`. It is compiled
by Emscripten and linked against the fuzzed Tachyon C++ core into a single WASM
module, so the page JavaScript and the C++ program share one WASM memory and one
ring engine. Both sides drive the rings through the exact same sanitized C ABI —
the C++ core is the single source of truth.

The browser build does not use POSIX shared memory or UNIX sockets. Those APIs
are unavailable in browsers, so the WASM path is a page-local Tachyon ring with
the same 64-byte message header, alignment, `type_id`, and skip-marker rules.

## Run

```bash
# from the repo root, make the Emscripten toolchain available:
source .emsdk/emsdk_env.sh     # run `bash ci/setup/install_emsdk.sh` first if needed

cd examples/browser_wasm
npm --prefix ../../bindings/js ci --ignore-scripts
npm --prefix ../../bindings/js run build:ts
npm ci 
npm run build:wasm             # Run the CMake target
npm run dev
```

Open the Vite URL, then use **Send To C++** or **Run Browser RTT Bench**.

## What the benchmark measures

**Send To C++** goes through the package wrapper. **Run Browser RTT Bench** calls the C ABI directly, skipping wrapper
copies and validation.

Neither is an IPC measurement. Both rings are page-local, both ends run on the same thread, and the round trip is a
sequence of WASM function calls: no process boundary, no futex, no cache coherency traffic. The numbers say what
JS-to-WASM call overhead costs, nothing more.

Results are batch-averaged because `performance.now()` is too coarse for individual sub-microsecond samples.
