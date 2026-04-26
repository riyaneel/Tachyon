# Tachyon C# bindings

[![NuGet](https://img.shields.io/nuget/v/TachyonIpc)](https://www.nuget.org/packages/TachyonIpc)

C# bindings for [Tachyon](https://github.com/riyaneel/tachyon), a bare-metal lock-free IPC. SPSC ring buffer over
POSIX shared memory with sub-100 ns p50 RTT.

The C core is accessed via `LibraryImport` P/Invoke (source-generated, AOT-compatible). The native shared library is
resolved at runtime by `NativeLoader` from the NuGet `runtimes/` layout, no manual installation required.

---

- [Requirements](#requirements)
- [Install](#install)
- [Quickstart](#quickstart)
- [API](#api)
- [Zero-copy pattern](#zero-copy-pattern)
- [Batch pattern](#batch-pattern)
- [Error handling](#error-handling)
- [Thread safety](#thread-safety)
- [NUMA binding](#numa-binding)
- [Type ID encoding](#type-id-encoding)
- [Limitations](#limitations)

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| .NET      | 8.0 or 10.0                               |
| Compiler  | GCC 14+ or Clang 17+ (source builds only) |

## Install

```bash
dotnet add package TachyonIpc
```

Prebuilt native libraries for Linux x64, Linux arm64, macOS x64, and macOS arm64 are bundled in the NuGet package.

## Quickstart

The consumer must start first, it owns the UNIX socket and the SHM arena.

```csharp
// consumer - start first, on a dedicated thread
using var listener = Bus.Listen("/tmp/demo.sock", 1024 * 1024);

using var rx = listener.ReceiveBlocking();
Console.WriteLine($"received {rx.Data.Length} bytes, type_id={rx.TypeId}");
```

```csharp
// producer
using var client = Bus.Connect("/tmp/demo.sock");

if (client.TryAcquireTx(64, out var tx))
{
    using (tx)
    {
        "hello tachyon"u8.CopyTo(tx.Buffer);
        tx.Commit(13, TypeId.Make(0, 1));
    }
    client.Flush();
}
```

`Bus` implements `IDisposable`. `using` guarantees the SHM arena is unmapped on scope exit.

## API

### Lifecycle

`Bus.Listen(socketPath, capacity)` creates a new SHM arena and binds a UNIX socket. Blocks until exactly one producer
calls `Connect`. The socket is discarded after the handshake. `capacity` must be a power of two.

`Bus.Connect(socketPath)` attaches to an existing arena. Returns immediately after the handshake.

`bus.Dispose()` unmaps shared memory. Idempotent.

`bus.Flush()` issues a store-release barrier and wakes a blocking receiver. Call after sequences of unflushed commits.
`TxGuard.Commit` calls it internally.

### TX

`bus.TryAcquireTx(maxPayloadSize, out TxGuard guard)` acquires a TX slot. Returns `false` if the ring is full.
Write into `guard.Buffer` (a `Span<byte>` over shared memory), then finalize with one of:

- `tx.Commit(actualSize, typeId)`: publish and flush.
- `tx.Rollback()`: cancel without publishing.

`TxGuard` is a `ref struct`. `Dispose()` rolls back automatically if `Commit` was not called.

```csharp
if (bus.TryAcquireTx(64, out var tx))
{
    using (tx)
    {
        payload.CopyTo(tx.Buffer);
        tx.Commit(payload.Length, TypeId.Make(0, 7));
    }
}
```

### RX single

`bus.TryReceive(out RxGuard guard)` is non-blocking. Returns `false` if the ring is empty.

`bus.ReceiveSpin(maxSpins, out RxGuard guard)` spins up to `maxSpins` times then returns `false`.

`bus.ReceiveBlocking(spinThreshold)` blocks until a message arrives. Never returns an empty guard.

Read via `guard.Data` (a `ReadOnlySpan<byte>`), then dispose. `RxGuard.Dispose()` commits automatically.

```csharp
if (bus.TryReceive(out var rx))
{
    using (rx)
    {
        Process(rx.Data, rx.MsgType);
    }
}
```

### RX batch

`bus.TryReceiveBatch(views, maxMsgs)` is non-blocking. Returns an empty `RxBatchGuard` if the ring is empty.

`bus.DrainBatch(views, maxMsgs, spinThreshold)` blocks until at least one message is available, then drains up to
`maxMsgs`. The `views` buffer must be `stackalloc TachyonMsgView[N]`.

`RxBatchGuard` supports `foreach` and indexed access via `batch[i]`. `Dispose()` commits all slots.

```csharp
const int N = 64;
var views = stackalloc TachyonMsgView[N];
using var batch = bus.DrainBatch(views, N);
foreach (var msg in batch)
{
    Process(msg.Data, msg.MsgType);
}
```

### Memory barrier

`Bus.MemoryBarrierAcquire()` issues an acquire barrier. Use before reading data received out-of-band.

### Ref counting

`bus.AddRef()` increments the native refcount. Advanced use only, required when sharing a handle across ownership
boundaries.

## Zero-copy pattern

`TxGuard.Buffer` and `RxGuard.Data` are `Span` and `ReadOnlySpan` over shared memory. They are valid only within the
guard's scope. Never store a reference to them beyond `Dispose()`.

```csharp
// Safe: copy before the guard closes.
if (bus.TryReceive(out var rx))
{
    using (rx)
    {
        byte[] snapshot = rx.Data.ToArray();
        Process(snapshot); // valid indefinitely
    }
}
```

`TxGuard` and `RxGuard` are `ref struct` types. They cannot be boxed, stored on the heap, or captured by lambdas.
This is enforced at compile time.

## Batch pattern

```csharp
// Batch TX - one flush for N messages.
foreach (var payload in payloads)
{
    if (bus.TryAcquireTx((nuint)payload.Length, out var tx))
    {
        using (tx)
        {
            payload.CopyTo(tx.Buffer);
            tx.Commit(payload.Length, typeId);
        }
    }
}
bus.Flush();
```

```csharp
// Batch RX - stackalloc, zero allocation.
const int N = 64;
var views = stackalloc TachyonMsgView[N];
using var batch = bus.DrainBatch(views, N);
foreach (var msg in batch)
{
    Process(msg.Data, msg.MsgType);
}
```

## Error handling

```csharp
try
{
    using var bus = Bus.Connect("/tmp/demo.sock");
    bus.TryAcquireTx(64, out var tx);
}
catch (TachyonException e) when (e.Error == TachyonError.AbiMismatch)
{
    throw new InvalidOperationException("Rebuild producer and consumer from the same Tachyon tag.", e);
}
catch (TachyonException e) when (e.Error == TachyonError.Full)
{
    // Ring buffer full - back off and retry.
}
catch (TachyonException e)
{
    Console.Error.WriteLine($"IPC error [{e.Error}]: {e.Message}");
}
```

| Error         | Trigger                                              |
|---------------|------------------------------------------------------|
| `AbiMismatch` | Handshake rejected, `TACHYON_MSG_ALIGNMENT` mismatch |
| `Full`        | Ring buffer full                                     |
| `System`      | Native syscall failure                               |

## Thread safety

`Bus` is not thread-safe. Each direction (TX or RX) must be used by at most one thread at a time. Tachyon is SPSC,
not MPSC.

`Bus.Listen` and `Bus.ReceiveBlocking` block the calling thread for their duration. Run them on a dedicated
`Thread` or `Task.Run` with a dedicated thread pool, not on the thread pool used by `async`/`await`.

## NUMA binding

```csharp
using var bus = Bus.Listen(path, 1024 * 1024);
bus.SetNumaNode(0); // bind SHM pages to NUMA node 0, before the first message
```

`SetNumaNode` uses `MPOL_PREFERRED + MPOL_MF_MOVE`. No-op on macOS.

## Type ID encoding

`typeId` is a `uint` split into two 16-bit halves since v0.4.0:

- bits [31:16]: routeId - reserved for RPC, must be 0 for now
- bits [15:0]: msgType - application-defined discriminator

`routeId = 0` exactly preserves v0.3.x semantics: `TypeId.Make(0, 42) == 42`.

```csharp
using TachyonIpc;

uint id = TypeId.Make(0, 42);   // == 42, identical to v0.3.x
TypeId.RouteId(id);             // 0
TypeId.MsgType(id);             // 42

tx.Commit(size, TypeId.Make(0, 42));

if (bus.TryReceive(out var rx))
{
    using (rx)
    {
        ushort mt = rx.MsgType;  // 42
    }
}
```

`routeId >= 1` is reserved for RPC. Do not use it on consumers.

## Limitations

**.NET 8 or 10 required.** `LibraryImport` source generation requires .NET 7+. Multi-targeting covers net8.0 (LTS)
and net10.0 (current LTS).
**Linux is the primary platform.** `SetNumaNode`, futex-based sleep, and `memfd_create` are Linux-specific. macOS is
supported at tier-2.
**SPSC only.** One producer, one consumer.
**No peer crash detection.** Blocking calls stall indefinitely if the remote process crashes.

## License

Apache 2.0
