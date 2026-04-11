# tachyon — Node.js bindings

[![npm](https://img.shields.io/npm/v/@tachyon-ipc/core)](https://www.npmjs.com/package/@tachyon-ipc/core)

Node.js bindings for [Tachyon](https://github.com/riyaneel/tachyon) — bare-metal lock-free IPC. SPSC ring buffer over
POSIX shared memory with sub-100 ns p50 RTT.

The C++ core is exposed via **N-API v8**. Prebuilds are provided for Linux x64. On other platforms the addon is
compiled from source at install time via `cmake-js`.

---

- [Requirements](#requirements)
- [Install](#install)
- [Quickstart](#quickstart)
- [API](#api)
- [Zero-copy pattern](#zero-copy-pattern)
- [Batch pattern](#batch-pattern)
- [Worker threads](#worker-threads)
- [Branded types](#branded-types)
- [Error handling](#error-handling)
- [Thread safety](#thread-safety)
- [NUMA binding](#numa-binding)
- [Prebuild vs compile](#prebuild-vs-compile)
- [Limitations](#limitations)

---

## Requirements

| Component  | Minimum                                   |
|------------|-------------------------------------------|
| OS         | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Node.js    | 20+                                       |
| TypeScript | 5.2+ (`using` keyword / ES2023 ERM)       |
| Compiler   | GCC 14+ or Clang 17+ (source builds only) |

## Install

```bash
npm install @tachyon-ipc/core
```

Prebuilds for Linux x64 are bundled. On other platforms, `cmake-js` compiles the addon from source. GCC 14+ or
Clang 17+ must be available on the build machine.

The package ships as **ESM** (`"type": "module"`). CommonJS consumers must use dynamic `import()`.

## Quickstart

The consumer must start first — it owns the UNIX socket and the SHM arena.

```typescript
// worker.ts — consumer side (must run on a Worker thread)
import {Bus} from '@tachyon-ipc/core';

using bus = Bus.listen('/tmp/demo.sock', 1 << 16);
const {data, typeId} = bus.recv();
console.log(`received ${data.byteLength} bytes, type_id=${typeId}`);
```

```typescript
// producer — main thread or any Worker
import {Bus} from '@tachyon-ipc/core';

using bus = Bus.connect('/tmp/demo.sock');
bus.send(Buffer.from('hello tachyon'), 1);
```

`Bus` implements `Disposable`. The `using` keyword (TypeScript 5.2+, ES2023 Explicit Resource Management) guarantees
`close()` is called on scope exit regardless of exceptions.

## API

### Lifecycle

`Bus.listen(socketPath, capacity)` creates a new SHM arena and binds a UNIX socket. **This call blocks the calling
thread** until exactly one producer calls `connect`. Always invoke it from a Worker thread to avoid stalling the main
event loop. `capacity` must be a strictly positive power of two.

`Bus.connect(socketPath)` attaches to an existing arena. Returns immediately after the handshake.

`bus.close()` unmaps shared memory. Safe to call multiple times. Prefer `using`.

`bus.flush()` publishes pending unflushed TX messages. `bus.send()` and `tx.commit()` call it internally.

### Standard API

`bus.send(data: Buffer | Uint8Array, typeId?: number)` copies the payload into the ring buffer, commits, and flushes.
`typeId` defaults to `0`.

`bus.recv(spinThreshold?: number): { data: Buffer; typeId: number }` blocks until a message is available. Returns a
heap-owned `Buffer` — safe to retain indefinitely. Retries transparently on EINTR.

### Zero-copy TX

`bus.acquireTx(maxSize: number): TxGuard` acquires a TX slot. Write into the slot via `TxGuard.bytes()`, which returns
a `TxSlot` (branded `Buffer`) pointing directly into shared memory. Finalize with one of:

- `tx.commit(actualSize, typeId)` — publish and flush.
- `tx.commitUnflushed(actualSize, typeId)` — publish without flushing; call `bus.flush()` after the batch.
- `tx.rollback()` — cancel without publishing.

`TxGuard` implements `Disposable`. Exiting a `using` block without an explicit commit triggers automatic rollback,
preventing indefinite producer lock starvation.

```typescript
using tx = bus.acquireTx(64);
payload.copy(tx.bytes());
tx.commit(payload.byteLength, 7);
```

### Zero-copy RX

`bus.acquireRx(spinThreshold?: number): RxGuard | null` blocks until a message is available. Returns `null` on EINTR
— the caller decides whether to retry. Read via `rx.data()`, `rx.typeId`, and `rx.actualSize`, then commit.

`RxGuard` implements `Disposable`. Exiting a `using` block without an explicit commit triggers automatic commit.

```typescript
const rx = bus.acquireRx();
if (rx === null) return; // EINTR
using _ = rx;
process(rx.data(), rx.typeId);
// commits on scope exit
```

### Batch RX

`bus.drainBatch(maxMsgs: number, spinThreshold?: number): RxBatch` blocks until at least one message is available,
then drains up to `maxMsgs` in a single N-API crossing. Use `batch.at(i)` for indexed access or `for...of` for
iteration. Call `batch.commit()` when done.

`RxBatch` implements `Disposable`. Exiting a `using` block triggers automatic commit.

```typescript
using batch = bus.drainBatch(64);
for (const msg of batch) {
    process(msg.data, msg.typeId, msg.size);
}
// auto-commits on scope exit — all ArrayBuffers are detached
```

## Zero-copy pattern

`tx.bytes()` and `rx.data()` return `Buffer` objects backed by a `noop_finalizer` — the N-API layer does not copy and
does not free the underlying shared memory. They are valid only until the corresponding `commit()`, `commitUnflushed()`,
or `rollback()` call.

On `batch.commit()`, the C++ layer **detaches** every `ArrayBuffer` backing the batch slots. Any cached reference will
immediately throw `TypeError: Cannot perform %TypedArray%.prototype.set on a detached ArrayBuffer` — a fail-fast
enforced at the V8 level, with zero SHM writes and zero cache invalidation.

```typescript
// Safe: copy before the guard closes.
using rx = bus.acquireRx()!;
const snapshot = Buffer.from(rx.data()); // heap copy
// rx.data() would throw after scope exit
process(snapshot); // valid indefinitely
```

## Batch pattern

```typescript
// Batch TX — one flush for N messages.
for (const payload of payloads) {
    using tx = bus.acquireTx(payload.byteLength);
    payload.copy(tx.bytes());
    tx.commitUnflushed(payload.byteLength, typeId);
}
bus.flush();
```

```typescript
// Batch RX — one N-API crossing for up to 64 messages.
using batch = bus.drainBatch(64);
for (const msg of batch) {
    process(msg.data, msg.typeId);
}
// auto-commits; all msg.data ArrayBuffers are detached
```

`drainBatch` crosses the N-API boundary exactly once per call. Draining 64 messages amortizes the ~15–20 ns per-crossing
overhead by 64×.

## Worker threads

`Bus.listen()`, `bus.recv()`, `bus.acquireRx()`, and `bus.drainBatch()` are synchronous blocking C calls. Invoking
them on the main thread blocks the V8 event loop for their full duration — timers, I/O callbacks, and GC do not run.

Always run the **consumer** on a dedicated Worker thread:

```typescript
// consumer-worker.ts
import {parentPort} from 'node:worker_threads';
import {Bus} from '@tachyon-ipc/core';

using bus = Bus.listen('/tmp/bus.sock', 1 << 16);
parentPort!.postMessage({type: 'ready'});

for (; ;) {
    const {data, typeId} = bus.recv();
    parentPort!.postMessage({data, typeId}, [data.buffer]);
}
```

```typescript
// main.ts
import {Worker} from 'node:worker_threads';
import {createRequire} from 'node:module';
import {Bus} from '@tachyon-ipc/core';

const TSX_ESM = createRequire(import.meta.url).resolve('tsx/esm');
const worker = new Worker(new URL('./consumer-worker.ts', import.meta.url), {
    execArgv: ['--import', TSX_ESM],
});

using bus = Bus.connect('/tmp/bus.sock');
bus.send(Buffer.from('ping'), 1);
```

Native addon handles (`tachyon_bus_t*`) cannot cross Worker thread boundaries — each side must hold its own `Bus`
instance.

## Branded types

`TxSlot` and `RxSlot` are nominal subtypes of `Buffer`:

```typescript
declare const txSlotBrand: unique symbol;
declare const rxSlotBrand: unique symbol;

export type TxSlot = Buffer & { readonly [txSlotBrand]: true };
export type RxSlot = Buffer & { readonly [rxSlotBrand]: true };
```

The brand symbols are module-private. `TxSlot` can only be produced by `TxGuard.bytes()`; `RxSlot` only by
`RxGuard.data()` or `RxMessage.data`. This makes it a compile-time contract: TypeScript's structural type system cannot
enforce the distinction between an arbitrary `Buffer` and a live SHM-backed slot — the brand does.

## Error handling

```typescript
import {isAbiMismatch, isFull, isPeerDead, isTachyonError} from '@tachyon-ipc/core';

try {
    using bus = Bus.connect('/tmp/demo.sock');
    bus.send(payload, 1);
} catch (err) {
    if (isAbiMismatch(err)) {
        throw new Error('Rebuild producer and consumer from the same Tachyon tag.');
    }
    if (isFull(err)) {
        // Ring buffer full — producer outpacing consumer.
    }
    if (isPeerDead(err)) {
        // Bus entered FATAL_ERROR — corrupted message header. Close immediately.
    }
    throw err;
}
```

| Guard              | Matches                                                             |
|--------------------|---------------------------------------------------------------------|
| `isTachyonError()` | Any error from the native binding (carries an `ERR_TACHYON_*` code) |
| `isAbiMismatch()`  | Handshake rejected — `TACHYON_MSG_ALIGNMENT` mismatch               |
| `isFull()`         | Ring buffer full — `ERR_TACHYON_FULL`                               |
| `isPeerDead()`     | Bus entered `TACHYON_STATE_FATAL_ERROR` (corrupted message header)  |

`PeerDeadError` is raised by the TypeScript layer (not the native binding) after polling `tachyon_get_state()`.
It carries code `ERR_TACHYON_UNKNOWN`.

## Thread safety

`Bus` is not thread-safe. Each direction (TX or RX) must be used by **at most one thread at a time** — Tachyon is SPSC,
not MPSC.

A Worker thread that blocks inside a Tachyon call parks its OS thread for the duration of the call. N consumers on N
Worker threads result in N parked OS threads. This is expected and bounded.

## NUMA binding

```typescript
using bus = Bus.listen(path, 1 << 16);
bus.setNumaNode(0); // bind SHM pages to NUMA node 0, before the first message
```

`setNumaNode` uses `MPOL_PREFERRED + MPOL_MF_MOVE`. No-op on macOS.

## Prebuild vs compile

The package ships a prebuilt `.node` addon for Linux x64 (glibc 2.31+). The addon is loaded at runtime via
`createRequire` from `build/Release/tachyon_node.node` or `build/Debug/tachyon_node.node`.

To force a source build:

```bash
npm install @tachyon-ipc/core --build-from-source
```

To rebuild after modifying the C++ core:

```bash
rm -rf build && npm run build:native
```

## Limitations

**Node.js 20+ required.** `import.meta.dirname` (used for addon path resolution) is available from Node 21.2+; on
Node 20 pass `--experimental-import-meta-resolve`.

**Blocking calls must run on Worker threads.** `Bus.listen()`, `recv()`, `acquireRx()`, and `drainBatch()` are
synchronous C calls that block the calling OS thread.

**ESM only.** The package ships with `"type": "module"`. CommonJS consumers must use `import()`.

**Linux is the primary platform.** `setNumaNode`, futex-based sleep, and `memfd_create` are Linux-specific.

**SPSC only.** One producer, one consumer.

**No peer crash detection.** If the producer crashes, blocking consumer calls stall indefinitely.

## License

Apache 2.0