# Browser Extension Native Tachyon Example

This example wires Tachyon into all three runtimes in the path: browser page,
Manifest V3 service worker, and native host. The page connects directly to the
extension service worker with `externally_connectable`; there is no
content-script bridge or fallback path. Each browser runtime uses a local
browser-WASM Tachyon ring before crossing the Chrome boundary, and the native
host forwards work through native Tachyon SPSC rings to a Rust worker thread.

Path:

```text
page Tachyon ring -> extension service worker external port
        -> service worker Tachyon rings -> connectNative port
        -> native host stdio -> native Tachyon request ring -> Rust worker
        -> native Tachyon reply ring -> native host stdio
        -> service worker Tachyon rings -> page Tachyon ring
```

The native messaging side uses the same optimizations as the C/Rust native
messaging references:

- persistent `chrome.runtime.connectNative()` instead of per-message process
  startup through `sendNativeMessage()`
- `message_serialization: "structured_clone"` for the page/service-worker port
  on Chrome versions that support it
- a long-running native host read loop
- direct 4-byte length-prefix stdin/stdout handling with one framed write per
  response
- no `serde_json` dependency in the host hot path
- fixed-shape JSON parsing for the browser boundary
- Tachyon zero-copy slots for the native host to worker hop
- an externally-connectable direct page port with no content-script hop
- a single in-flight benchmark path with no extra local buffering around Chrome
  or native messaging
- committed browser-WASM Tachyon runtime files vendored into the extension so
  the service worker and page run the same Tachyon WASM transport

Chrome still requires native messaging messages to be valid JSON over stdio, so
this example is a bridge tier, not a replacement for Tachyon shared memory.

## Latest Local Benchmark

Measured on macOS with Chrome Canary 150.0.7834.0, `message_serialization:
"structured_clone"`, a pinned service worker target, one persistent
`connectNative()` port, and 3,000 single in-flight RTT samples:

| path | p50 | p90 | p99 | mean | throughput |
| --- | ---: | ---: | ---: | ---: | ---: |
| full page -> service worker -> native host -> Tachyon RTT | 295.0 us | 410.0 us | 610.0 us | 309.6 us | 3,230 RTT/sec |
| page -> service worker echo, no native host | 160.0 us | 220.0 us | 685.0 us | 192.2 us | 5,202 RTT/sec |
| page -> service worker -> native host, service-worker Tachyon rings bypassed | 290.0 us | 375.0 us | 505.0 us | 303.4 us | 3,296 RTT/sec |
| direct native host stdio -> native Tachyon worker RTT | 6.7 us | 17.5 us | 43.5 us | 12.8 us | 78,110 RTT/sec |

The component cost is dominated by Chrome boundaries:

- page Tachyon plus the external page/service-worker `runtime.Port` round trip:
  about 160 us p50
- adding `connectNative()` and Chrome's native messaging JSON/stdio bridge:
  roughly another 130 us p50
- service-worker-local Tachyon WASM rings: about 5 us p50 in this run
- direct native host framing plus native Tachyon worker: about 7 us p50

The figures are not perfectly additive because Chrome scheduling and service
worker dispatch add run-to-run variance, but the order of magnitude is stable:
the page/service-worker and service-worker/native messaging boundaries dominate
the RTT, not Tachyon itself.

## Build

```bash
cd examples/browser_extension_native_tachyon/native_host
cargo build --release
```

## Install the Native Host Manifest

Load `examples/browser_extension_native_tachyon/extension` as an unpacked
extension in Chrome, then copy its extension id.

```bash
cd examples/browser_extension_native_tachyon
node scripts/install-host.mjs chrome <extension-id>
```

For Chrome for Testing:

```bash
node scripts/install-host.mjs chrome_for_testing <extension-id>
```

## Run

Serve this example directory from localhost so both the page and vendored
browser-WASM Tachyon package are available:

```bash
cd examples/browser_extension_native_tachyon
python3 -m http.server 8080
```

Open the page with the extension id in the query string for the direct path:

```text
http://127.0.0.1:8080/page/?extensionId=<extension-id>
```

Click **Run RTT Bench** to measure the complete path.

The page reports the full browser/extension/native round trip. Measure the
native host directly when you need to isolate the native Tachyon hop from
Chrome's page, extension, and native messaging boundaries.
