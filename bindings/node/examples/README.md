# Node.js quickstart

Two-terminal SPSC demo using the Node binding. The consumer listens on a UNIX
socket and owns the shared-memory arena; the producer connects, sends 100,000
messages plus a sentinel, then exits.

## Build the binding (one-time)

From the repo root:

```bash
bash ci/vendor.sh node                                # copy core/ into bindings/node/src/native/_core_local
CXX=g++-14 CC=gcc-14 npm --prefix bindings/node install   # also builds the native addon
npm --prefix bindings/node run build:ts               # tsc -> dist/
```

Requires gcc 14+ or clang 17+ (same toolchain as the core).

## Run

Terminal 1 — consumer (start first):

```bash
node bindings/node/examples/consumer.mjs
```

Terminal 2 — producer:

```bash
node bindings/node/examples/producer.mjs
```

## What to expect

```
[consumer] listen /tmp/tachyon_node_demo.sock (cap=1048576)
[consumer] producer connected
[consumer] #1 type=1 "tick 0"
...
[consumer] received 100,000 messages in ~185 ms (~540,000 msg/s)
```

The producer's `sendBackpressured()` helper retries on `ERR_TACHYON_FULL` —
needed because Node's FFI overhead makes per-message `send`/`recv` cost ~1–2 µs,
so the producer can outpace the consumer even with a 1 MB ring. This is the
right pattern for any production use of `bus.send()`: handle backpressure
explicitly rather than crashing.

The headline throughput (~500K msg/s) is dominated by the Node ↔ N-API boundary,
not the underlying Tachyon transport (which the C++ benchmarks clock at ~50 ns
p50 = 20M RTT/s). To approach the transport floor, use `bus.acquireTx` /
`bus.drainBatch` from a Worker thread — see `bindings/node/test/` for the
patterns.
