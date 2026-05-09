# Tachyon Go bindings

[![CI](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml/badge.svg)](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml)
[![Go Reference](https://pkg.go.dev/badge/github.com/riyaneel/tachyon/bindings/go.svg)](https://pkg.go.dev/github.com/riyaneel/tachyon/bindings/go)

Go bindings for [Tachyon](https://github.com/riyaneel/tachyon), a bare-metal lock-free IPC. SPSC ring buffer over POSIX
shared memory with sub-100 ns p50 RTT.

The C++ core is compiled at build time via CGO. No shared library is required at runtime.

---

- [Requirements](#requirements)
- [Install](#install)
- [Quickstart](#quickstart)
- [API](#api)
- [Zero-copy lifetime](#zero-copy-lifetime)
- [Batch pattern](#batch-pattern)
- [RPC](#rpc)
- [Error handling](#error-handling)
- [Thread safety](#thread-safety)
- [NUMA binding](#numa-binding)
- [Limitations](#limitations)

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Go        | 1.23+                                     |
| Compiler  | GCC 14+ or Clang 17+                      |

CGO must be enabled (`CGO_ENABLED=1`, the default). The C++ compiler is selected via the `CXX` environment variable.

## Install

```bash
go get github.com/riyaneel/tachyon/bindings/go@v0.5.1
```

Set `CXX` before building so CGO selects a compatible compiler:

```bash
CXX=g++-14 CC=gcc-14 go build ./...
```

## Quickstart

The consumer must start first, it owns the UNIX socket and the SHM arena.

```go
// consumer - start first
bus, err := tachyon.Listen("/tmp/demo.sock", 1<<16)
if err != nil { log.Fatal(err) }
defer bus.Close()

data, typeID, err := bus.Recv(10_000)
if err != nil { log.Fatal(err) }
fmt.Printf("received %q type_id=%d\n", data, typeID)
```

```go
// producer - start after the consumer
bus, err := tachyon.Connect("/tmp/demo.sock")
if err != nil { log.Fatal(err) }
defer bus.Close()

if err := bus.Send([]byte("hello tachyon"), 1); err != nil {
    log.Fatal(err)
}
```

See [`examples/go_producer_go_consumer/`](../../examples/go_producer_go_consumer) for a complete example with batch TX,
batch RX, sequence validation, and a sentinel shutdown signal.

## API

### Lifecycle

`Listen(socketPath, capacity)` creates a new SHM arena and binds a UNIX socket. It blocks until exactly one producer
calls `Connect`. The socket is discarded after the handshake; all subsequent I/O runs through shared memory.

`Connect(socketPath)` attaches to an existing arena. It returns immediately after the handshake, it does not wait for
the consumer to call `Listen`. Use a retry loop if the consumer may not be ready.

`Bus.Close()` unmaps the shared memory and releases all resources. Safe to call multiple times.

`Bus.Flush()` publishes pending TX messages. Must be called after `CommitUnflushed` sequences to make batched messages
visible to the consumer. `Send` and `Commit` call `Flush` internally.

`Bus.SetNumaNode(nodeID)` binds the SHM pages to a specific NUMA node. See [NUMA binding](#numa-binding).

### Standard API

`Bus.Send(data, typeID)` copies data into the ring buffer, commits, and flushes. Blocks if the ring is full. `typeID` is
an application-defined discriminator read back via `RxGuard.TypeID()`.

`Bus.Recv(spinThreshold)` blocks until a message is available, copies the payload into a new`[]byte`, and returns
`(data, typeID, error)`. The returned slice is owned by the caller and remains valid indefinitely.

### Zero-copy TX

`Bus.AcquireTx(maxSize)` acquires a TX slot and returns a `*TxGuard`. Write into the slot via `TxGuard.Bytes()`, then
call one of:

- `TxGuard.Commit(actualSize, typeID)`: publish and flush. Use for single-message sends.
- `TxGuard.CommitUnflushed(actualSize, typeID)`: publish without flushing. Use when batch-sending; call `Bus.Flush()`
  after the last message.
- `TxGuard.Rollback()`: cancel without publishing. The slot is returned to the producer.

Every `AcquireTx` must be followed by exactly one `Commit`, `CommitUnflushed`, or `Rollback`. Failing to do so holds the
producer lock indefinitely.

### Zero-copy RX

`Bus.AcquireRx(spinThreshold)` blocks until a message is available and returns a `*RxGuard`. Read the payload via
`RxGuard.Data()`, then call `RxGuard.Commit()` to release the slot.

Every `AcquireRx` must be followed by exactly one `Commit`.

### Batch RX

`Bus.DrainBatch(maxMsgs, spinThreshold)` blocks until at least one message is available, then drains up to `maxMsgs` in
a single CGO crossing. Each CGO call costs ~50–100 ns regardless of how many messages are drained. Batching 64 messages
reduces per-message overhead by 64×.

`Bus.TryDrainBatch(maxMsgs)` is the non-blocking variant. It returns `(nil, nil)` immediately if no messages are
available. Use it in polling loops or when multiplexing over multiple buses.

`RxBatch.Iter()` returns a range-over-func iterator (Go 1.23+):

```go
for msg := range batch.Iter() {
    process(msg.Data())
}
batch.Commit()
```

`RxBatch.Commit()` releases all slots atomically. All `RxMsg.Data()` slices are invalid after this call. Safe to call
multiple times (no-op after the first).

### Errors

`TachyonError` carries a numeric `Code` and a human-readable `Message`. Use the sentinel functions for structured
inspection rather than comparing `Code` values directly:

- `IsABIMismatch(err)`: producer and consumer compiled with incompatible versions.
- `IsInterrupted(err)`: signal interrupted a blocking call (internally retried in most cases).
- `IsFull(err)`: ring buffer full (relevant only when calling the C API directly).

## Zero-copy lifetime

`RxGuard.Data()`, `TxGuard.Bytes()`, and `RxMsg.Data()` return slices that point **directly into shared memory**. They
are valid only until the corresponding `Commit` or `Rollback` call. Retaining a reference past that point is undefined
behavior.

```go
// Copy before Commit if the data must outlive the guard.
guard, _ := bus.AcquireRx(10_000)
dst := make([]byte, guard.Size())
copy(dst, guard.Data())
guard.Commit()
// dst is valid here; guard.Data() is not.
```

```go
// All RxMsg.Data() slices are invalid after RxBatch.Commit().
batch, _ := bus.DrainBatch(64, 10_000)
for msg := range batch.Iter() {
    process(msg.Data())   // valid here
}
batch.Commit()
// msg.Data() is invalid here.
```

If the GC collects a guard or batch without an explicit `Commit` or `Rollback`, the finalizer calls it automatically.
This is a safety net, the nominal path is always an explicit call.

## Batch pattern

```go
// Batch TX - one flush after all messages.
for _, p := range payloads {
    g, _ := bus.AcquireTx(len(p))
    copy(g.Bytes(), p)
    g.CommitUnflushed(len(p), typeID)
}
bus.Flush()
```

```go
// Batch RX - one CGO crossing for up to 64 messages.
batch, err := bus.DrainBatch(64, 10_000)
if err != nil { return err }
for msg := range batch.Iter() {
    process(msg.Data())
}
batch.Commit()
```

## RPC

```go
// callee - start first
callee, _ := tachyon.ListenRpc("/tmp/rpc.sock", 1<<16, 1<<16)
defer callee.Close()

req, _ := callee.Serve(10_000)
cid := req.CorrelationID()
data := append([]byte(nil), req.Data()...)
req.Commit()
callee.Reply(cid, data, 2)

// caller
caller, _ := tachyon.ConnectRpc("/tmp/rpc.sock")
defer caller.Close()

cid, _ := caller.Call([]byte("ping"), 1)
resp, _ := caller.Wait(cid, 10_000)
fmt.Printf("reply: %q\n", resp.Data())
resp.Commit()
```

`RpcRxGuard.Data()` points directly into shared memory and is valid only until `Commit()`.
Copy before `Commit()` if the data must outlive the guard.
The finalizer calls `Commit()` if the guard is collected without an explicit call.

## Error handling

```go
bus, err := tachyon.Connect("/tmp/demo.sock")
if err != nil {
    if tachyon.IsABIMismatch(err) {
        log.Fatal("rebuild producer and consumer from the same Tachyon tag")
    }
    log.Fatal(err)
}
```

## Thread safety

`Bus` is safe to pass between goroutines. Each direction (TX or RX) must be used by **at most one goroutine at a time**.
Tachyon is SPSC, not MPSC.

A goroutine that blocks inside a Tachyon call parks its OS thread for the duration of the call. N goroutines blocking on
N different buses results in N OS threads. This is expected and bounded; account for it in capacity planning.

Do not call `runtime.LockOSThread()` inside code that calls into this package. Thread affinity is the caller's
responsibility, not the binding's.

## NUMA binding

If producer and consumer are on different NUMA nodes, all ring buffer accesses cross the interconnect. Call
`SetNumaNode()` immediately after `Listen()` or `Connect()`, before the first message, to migrate the SHM pages to the
correct node.

```go
bus, _ := tachyon.Listen(path, capacity)
if err := bus.SetNumaNode(0); err != nil {
    log.Printf("NUMA binding failed (non-fatal): %v", err)
}
```

`SetNumaNode` uses `MPOL_PREFERRED + MPOL_MF_MOVE`, it prefers the requested node but falls back rather than failing
hard. It is a no-op on macOS.

## Type ID encoding

`typeID` is a `uint32` split into two 16-bit halves since v0.4.0:

```
bits [31:16] RouteID: reserved for RPC, must be 0 for now
bits [15:0]  MsgType: application-defined discriminator
```

`RouteID = 0` exactly preserves v0.3.x semantics: `MakeTypeID(0, 42) == 42`.

```go
// encode
id := tachyon.MakeTypeID(0, 42) // == 42, identical to v0.3.x

// decode
route := tachyon.RouteID(id) // 0
mt := tachyon.MsgType(id) // 42

// send and receive
bus.Send(data, tachyon.MakeTypeID(0, 42))

data, typeID, _ := bus.Recv(10_000)
fmt.Println(tachyon.MsgType(typeID)) // 42
```

`RouteID >= 1` is reserved for RPC. Do not use it on consumers.

## Limitations

**CGO is required.** `CGO_ENABLED=0` does not compile. The C++ core is compiled at build time; GCC 14+ or Clang 17+ must
be available on the build machine.

**`-march=native` by default.** Binaries are not portable across microarchitectures. To produce a portable binary,
override `CGO_CXXFLAGS`:

```bash
CGO_CXXFLAGS="-std=c++23 -O3 -fno-exceptions -fno-rtti" go build ./...
```

**Linux and macOS only.** The build tag `//go:build linux || darwin` excludes other platforms.

**SPSC only.** One producer, one consumer. For fan-in workloads, use N independent buses or an application-level
multiplexer.

**No peer crash detection.** If the producer crashes, the consumer blocks indefinitely waiting for the next message. Use
an external mechanism, `pidfd`, `SIGCHLD`, or a dedicated health-check busn if dead-peer detection is required. See [
`INTEGRATION.md`](../../INTEGRATION.md) for supervision patterns.
