# Tachyon Kotlin bindings

Kotlin bindings for [Tachyon](https://github.com/riyaneel/tachyon), a bare-metal lock-free IPC. SPSC ring buffer over
POSIX shared memory with sub-100 ns p50 RTT.

Built on top of the Java Panama FFM layer. `Bus` wraps `TachyonBus` with idiomatic Kotlin surface: suspendable `send`,
a `Flow<Message>` receiver, and `use {}` blocks for resource safety. The underlying FFM `MethodHandle` dispatch layer is
shared with the Java binding, zero additional overhead.

---

- [Requirements](#requirements)
- [Install](#install)
- [Quickstart](#quickstart)
- [API](#api)
- [Zero-copy pattern](#zero-copy-pattern)
- [Batch pattern](#batch-pattern)
- [Coroutines](#coroutines)
- [Error handling](#error-handling)
- [Thread safety](#thread-safety)
- [NUMA binding](#numa-binding)
- [Type ID encoding](#type-id-encoding)
- [Benchmark results](#benchmark-results)
- [Limitations](#limitations)

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Kotlin    | 2.0+                                      |
| JVM       | 21+ (Panama FFM GA)                       |
| Compiler  | GCC 14+ or Clang 17+ (source builds only) |
| Build     | Gradle 8+                                 |

## Install

```kotlin
// build.gradle.kts
dependencies {
    implementation("dev.tachyon-ipc:tachyon-kotlin:0.3.5")
}
```

Add `--enable-native-access=ALL-UNNAMED` to `gradle.properties`:

```properties
org.gradle.jvmargs=--enable-native-access=ALL-UNNAMED
```

## Quickstart

The consumer must start first, it owns the UNIX socket and the SHM arena.

```kotlin
// consumer - on a dedicated coroutine dispatcher backed by a platform thread
Bus.listen("/tmp/demo.sock", 1L shl 16).use { bus ->
    bus.receive().collect { msg ->
        println("received ${msg.size} bytes, type_id=${msg.typeId}")
    }
}
```

```kotlin
// producer
Bus.connect("/tmp/demo.sock").use { bus ->
    bus.send("hello tachyon".toByteArray(), typeId = 1)
}
```

`Bus` implements `AutoCloseable`. The `use {}` block guarantees the SHM arena is unmapped on exit.

## API

### Lifecycle

`Bus.listen(path, capacity)` creates a new SHM arena and binds a UNIX socket. Blocks until exactly one producer calls
`connect`. `capacity` must be a strictly positive power of two passed as `Long`.

`Bus.connect(path)` attaches to an existing arena. Returns immediately after the handshake.

`bus.close()` unmaps shared memory. Safe to call multiple times. Prefer `use {}`.

`bus.flush()` publishes pending unflushed TX messages. Required after sequences of `TxGuard.commitUnflushed`.
`Bus.send()` and `TxGuard.commit()` call it internally.

### Suspendable send

`bus.send(data: ByteArray, typeId: Int = 0)` is a suspending function. It copies `data` into the ring buffer,
commits, and flushes. If the ring is temporarily full (`BufferFullException`), it calls `yield()` and retries, it never
blocks a carrier thread.

```kotlin
coroutineScope {
    launch(tachyonDispatcher) {
        Bus.connect("/tmp/demo.sock").use { bus ->
            bus.send("ping".toByteArray(), typeId = 1)
        }
    }
}
```

### Flow receiver

`bus.receive(spinThreshold, onEmpty)` converts the blocking C receiver into a cold `Flow<Message>`. It runs on
`Dispatchers.IO` and is cancellable. Each emitted `Message` holds a heap-owned `ByteArray`, safe to retain indefinitely.

```kotlin
Bus.listen("/tmp/demo.sock", 1L shl 16).use { bus ->
    bus.receive(spinThreshold = 1_000)
        .cancellable()
        .collect { msg ->
            process(msg.data, msg.typeId)
        }
}
```

`onEmpty` is called when `acquireRx` returns `null` (EINTR). The default is `yield()`. Override it to implement custom
back-off:

```kotlin
bus.receive(onEmpty = { delay(1) }).collect { … }
```

### Zero-copy TX

`bus.acquireTx(maxSize: Long): TxGuard` acquires an exclusive TX slot. Write into the slot via `TxGuard.getData()`,
which returns a `MemorySegment` pointing directly into shared memory. Finalize with one of:

- `tx.commit(actualSize, typeId)`: publish and flush.
- `tx.commitUnflushed(actualSize, typeId)`: publish without flushing; call `bus.flush()` after the batch.
- `tx.rollback()`: cancel without publishing.

`TxGuard` implements `AutoCloseable`. Exiting a `use {}` block without an explicit commit triggers automatic rollback.

```kotlin
bus.acquireTx(64L).use { tx ->
    MemorySegment.copy(MemorySegment.ofArray(payload), 0L, tx.data, 0L, payload.size.toLong())
    tx.commit(payload.size.toLong(), 7)
}
```

### Zero-copy RX

`bus.acquireRx(spinThreshold: Int): RxGuard?` blocks until a message is available, returning `null` on EINTR.
Read via `RxGuard.getData()`, `RxGuard.getTypeId()`, and `RxGuard.getActualSize()`, then commit.

`RxGuard` implements `AutoCloseable`. Exiting a `use {}` block without an explicit commit triggers automatic commit.

```kotlin
bus.acquireRx(10_000)?.use { rx ->
    val snapshot = rx.data.toArray(ValueLayout.JAVA_BYTE)
    process(snapshot, rx.typeId)
}
```

### Batch RX

`bus.drainBatch(maxMsgs: Int, spinThreshold: Int): RxBatchGuard` drains up to `maxMsgs` messages in a single FFM
crossing. `RxBatchGuard` implements `AutoCloseable` and `Iterable<RxMsgView>`.

```kotlin
bus.drainBatch(64, 10_000).use { batch ->
    for (msg in batch) {
        process(msg.data, msg.typeId)
    }
} // commits all slots on exit
```

## Zero-copy pattern

`TxGuard.getData()` and `RxGuard.getData()` return `MemorySegment` objects pointing **directly into shared memory**.
They are valid only until the corresponding `commit()`, `commitUnflushed()`, or `rollback()` call. Retaining a reference
past that point is undefined behavior.

```kotlin
// Safe: copy before the guard closes.
bus.acquireRx(10_000)?.use { rx ->
    val snapshot = rx.data.toArray(ValueLayout.JAVA_BYTE) // heap copy
    process(snapshot) // valid indefinitely
} // rx.data is invalid after this point
```

After `RxBatchGuard.commit()`, every `RxMsgView` is explicitly invalidated by the Java layer and `getData()` throws
`IllegalStateException` before any native access.

## Batch pattern

```kotlin
// Batch TX - one flush for N messages.
payloads.forEach { payload ->
    bus.acquireTx(payload.size.toLong()).use { tx ->
        MemorySegment.copy(MemorySegment.ofArray(payload), 0L, tx.data, 0L, payload.size.toLong())
        tx.commitUnflushed(payload.size.toLong(), typeId)
    }
}
bus.flush()
```

```kotlin
// Batch RX - one FFM crossing for up to 64 messages.
bus.drainBatch(64, 10_000).use { batch ->
    for (msg in batch) {
        process(msg.data, msg.typeId)
    }
}
```

## Coroutines

`Bus.send` is already a suspending function and handles back-pressure transparently via `yield()`. For the consumer
side, `bus.receive()` returns a `Flow<Message>` that runs on `Dispatchers.IO` and is safe to collect from any coroutine.

Blocking Tachyon calls (`Bus.listen`, `bus.acquireRx`, `bus.drainBatch`) park the calling OS thread for their duration.
To keep the coroutine scheduler unaffected, dispatch them on a dedicated single-threaded executor:

```kotlin
val tachyonDispatcher = Executors.newSingleThreadExecutor(
    Thread.ofPlatform().name("tachyon-consumer").factory()
).asCoroutineDispatcher()

launch(tachyonDispatcher) {
    Bus.listen("/tmp/demo.sock", 1L shl 16).use { bus ->
        bus.receive().collect { msg -> channel.send(msg) }
    }
}
```

Do not collect `bus.receive()` on `Dispatchers.Default`, a parked OS thread starves other coroutines sharing the
same carrier.

## Error handling

```kotlin
try {
    Bus.connect("/tmp/demo.sock").use { bus ->
        bus.send(payload, typeId = 1)
    }
} catch (e: AbiMismatchException) {
    error("Rebuild producer and consumer from the same Tachyon tag.")
} catch (e: BufferFullException) {
    // Ring buffer full - bus.send() handles this automatically via yield() + retry.
} catch (e: TachyonException) {
    logger.error("IPC error [{}]: {}", e.code, e.message)
}
```

| Exception              | Trigger                                                    |
|------------------------|------------------------------------------------------------|
| `AbiMismatchException` | Handshake rejected - `TACHYON_MSG_ALIGNMENT` mismatch      |
| `BufferFullException`  | Ring buffer full; `bus.send` handles this automatically    |
| `PeerDeadException`    | Bus entered `TACHYON_STATE_FATAL_ERROR`; close immediately |
| `TachyonException`     | Base class for all native errors                           |

## Thread safety

`Bus` is not thread-safe. Each direction (TX or RX) must be used by **at most one thread at a time**. Tachyon is SPSC,
not MPSC.

Kotlin coroutines scheduled on a single-threaded dispatcher are safe. Do not share a `Bus` instance across multiple
coroutines running concurrently, even if they appear sequentially ordered.

## NUMA binding

```kotlin
Bus.listen(path, 1L shl 16).use { bus ->
    bus.setNumaNode(0) // bind SHM pages to NUMA node 0, before the first message
    bus.receive().collect { … }
}
```

`setNumaNode` uses `MPOL_PREFERRED + MPOL_MF_MOVE`. No-op on macOS.

## Type ID encoding

`typeId` is an `Int` (uint32) split into two 16-bit halves since v0.4.0:

```kotlin
import dev.tachyon_ipc.TypeId

val id = TypeId.of(0, 42) // == 42, identical to v0.3.x
TypeId.routeId(id) // 0
TypeId.msgType(id) // 42

bus.send(data, typeId = TypeId.of(0, 42))

bus.acquireRx(10_000)?.use { rx ->
    val mt = TypeId.msgType(rx.getTypeId()) // 42
}
```

`routeId >= 1` is reserved for RPC. Do not use it on consumers.

## Benchmark results

Measured on Linux 6.19, Intel Core i7-12650H, 64 GiB DDR5-5600 SODIMM, JDK 21.0.10, Kotlin 2.2.21.

| Benchmark              | Score | Unit  |
|------------------------|-------|-------|
| Throughput (acquireTx) | ~76   | ns/op |
| RTT ping-pong p50      | ~739  | ns    |

The Kotlin wrapper adds zero overhead over the Java FFM dispatch layer. Figures are identical to the Java binding.

## Limitations

**JVM 21+ required.** Panama FFM graduated to GA in JDK 21.

**`--enable-native-access=ALL-UNNAMED` is mandatory.** Without this flag, FFM `MethodHandle` lookup throws
`IllegalCallerException` at startup.

**Linux is the primary platform.** `setNumaNode`, futex-based sleep, and `memfd_create` are Linux-specific.

**SPSC only.** One producer, one consumer.

**No peer crash detection.** Blocking calls stall indefinitely if the remote process crashes.

## License

Apache 2.0
