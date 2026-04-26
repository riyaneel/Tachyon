# Tachyon Java bindings

Java bindings for [Tachyon](https://github.com/riyaneel/tachyon), a bare-metal lock-free IPC. SPSC ring buffer over
POSIX shared memory with sub-100 ns p50 RTT.

The C core is accessed via **Java 21 Panama FFM** (`java.lang.foreign`). No JNI, no JNA. The native shared library is
extracted from the classpath at startup by `NativeLoader` and linked via `System.load`, no manual installation step
required.

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
- [Benchmark results](#benchmark-results)
- [Limitations](#limitations)

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Java      | 21+ (Panama FFM GA)                       |
| Compiler  | GCC 14+ or Clang 17+ (source builds only) |
| Build     | Gradle 8+                                 |

## Install

```kotlin
// build.gradle.kts
dependencies {
    implementation("dev.tachyon-ipc:tachyon-java:0.4.1")
}
```

The `--enable-native-access=ALL-UNNAMED` JVM flag is required. Add it to `gradle.properties`:

```properties
org.gradle.jvmargs=--enable-native-access=ALL-UNNAMED
```

For application launchers:

```bash
java --enable-native-access=ALL-UNNAMED -jar app.jar
```

## Quickstart

The consumer must start first, it owns the UNIX socket and the SHM arena.

```java
// consumer - start first, on a dedicated thread
try (var bus = TachyonBus.listen("/tmp/demo.sock", 1 << 16)) {
    try (var rx = bus.acquireRx(10_000)) {
        if (rx != null) {
            byte[] payload = rx.getData().toArray(ValueLayout.JAVA_BYTE);
            System.out.printf("received %d bytes, type_id=%d%n", rx.getActualSize(), rx.getTypeId());
        }
    } // auto-commits on exit
}
```

```java
// producer
try (var bus = TachyonBus.connect("/tmp/demo.sock")) {
    byte[] data = "hello tachyon".getBytes(StandardCharsets.UTF_8);
    try (var tx = bus.acquireTx(data.length)) {
        MemorySegment.copy(MemorySegment.ofArray(data), 0L, tx.getData(), 0L, data.length);
        tx.commit(data.length, 1);
    }
}
```

`TachyonBus` implements `AutoCloseable`. `try-with-resources` guarantees the SHM arena is unmapped and the file
descriptor is released regardless of exceptions.

## API

### Lifecycle

`TachyonBus.listen(path, capacity)` creates a new SHM arena and binds a UNIX socket. Blocks until exactly one
producer calls `connect`. The socket is discarded after the handshake; all subsequent I/O runs through shared memory.
`capacity` must be a strictly positive power of two.

`TachyonBus.connect(path)` attaches to an existing arena. Returns immediately after the handshake.

`bus.close()` unmaps shared memory and releases all resources. Safe to call multiple times. Always prefer
`try-with-resources`.

`bus.flush()` publishes pending unflushed TX messages. Must be called after `TxGuard.commitUnflushed` sequences.
`TxGuard.commit()` calls it internally.

### Zero-copy TX

`bus.acquireTx(maxSize)` acquires an exclusive TX slot and returns a `TxGuard`. Write into the slot via
`TxGuard.getData()`, which returns a `MemorySegment` pointing directly into shared memory. Finalize with one of:

- `tx.commit(actualSize, typeId)`: publish and flush.
- `tx.commitUnflushed(actualSize, typeId)`: publish without flushing. Call `bus.flush()` after the last message.
- `tx.rollback()`: cancel without publishing.

`TxGuard` implements `AutoCloseable`. Exiting a `try-with-resources` block without an explicit commit triggers automatic
rollback, preventing indefinite producer lock starvation.

```java
try (var tx = bus.acquireTx(64)) {
    MemorySegment.copy(MemorySegment.ofArray(payload), 0L, tx.getData(), 0L, payload.length);
    tx.commit(payload.length, 7);
}
```

### Zero-copy RX

`bus.acquireRx(spinThreshold)` blocks until a message is available and returns an `RxGuard`, or `null` on `EINTR`.
Read via `RxGuard.getData()`, `RxGuard.getTypeId()`, and `RxGuard.getActualSize()`, then commit.

`RxGuard` implements `AutoCloseable`. Exiting a `try-with-resources` block without an explicit commit triggers automatic
commit, advancing the consumer head.

```java
try (var rx = bus.acquireRx(10_000)) {
    if (rx == null) return; // EINTR - retry
    byte[] copy = rx.getData().toArray(ValueLayout.JAVA_BYTE);
    process(copy, rx.getTypeId());
} // auto-commits on exit
```

### Batch RX

`bus.drainBatch(maxMsgs, spinThreshold)` blocks until at least one message is available, then drains up to `maxMsgs`
in a single FFM crossing. Returns an `RxBatchGuard` backed by a confined `Arena` holding the native
`tachyon_msg_view_t[]` struct array.

`RxBatchGuard` implements `AutoCloseable` and `Iterable<RxMsgView>`. `getCount()` returns the number of messages
drained. `get(index)` retrieves a specific `RxMsgView`. `commit()` advances the consumer head for all messages in one
FFM crossing and closes the confined arena.

```java
try (var batch = bus.drainBatch(64, 10_000)) {
    for (var msg : batch) {
        process(msg.getData(), msg.getTypeId());
    }
} // auto-commits on exit, all RxMsgView segments are invalidated
```

## Zero-copy pattern

`TxGuard.getData()` and `RxGuard.getData()` return `MemorySegment` objects pointing **directly into shared memory**.
They are valid only until the corresponding `commit()`, `commitUnflushed()`, or `rollback()` call.

After `RxBatchGuard.commit()`, every `RxMsgView` is explicitly invalidated and `getData()` throws
`IllegalStateException`. This is enforced at the Java level before any native memory access.

The Panama FFM segments are backed by a confined `Arena` scoped to the guard lifetime. Any out-of-scope access throws
`IllegalStateException` at the JVM level before reaching native memory.

```java
// Safe: copy before the guard closes.
try (var rx = bus.acquireRx(10_000)) {
    if (rx == null) return;
    byte[] snapshot = rx.getData().toArray(ValueLayout.JAVA_BYTE);
    process(snapshot); // valid indefinitely
} // rx.getData() is invalid after this point
```

## Batch pattern

```java
// Batch TX - one flush for N messages.
for (byte[] payload : payloads) {
    try (var tx = bus.acquireTx(payload.length)) {
        MemorySegment.copy(MemorySegment.ofArray(payload), 0L, tx.getData(), 0L, payload.length);
        tx.commitUnflushed(payload.length, typeId);
    }
}
bus.flush();
```

```java
// Batch RX - one FFM crossing for up to 64 messages.
try (var batch = bus.drainBatch(64, 10_000)) {
    for (var msg : batch) {
        process(msg.getData(), msg.getTypeId());
    }
} // commits all slots on exit
```

## Error handling

All errors surface as `TachyonException` subclasses. Use typed subclasses for structured inspection:

```java
try {
    var bus = TachyonBus.connect("/tmp/demo.sock");
} catch (AbiMismatchException e) {
    throw new IllegalStateException("Rebuild producer and consumer from the same Tachyon tag.", e);
} catch (BufferFullException e) {
    // Ring buffer full - back off and retry.
} catch (TachyonException e) {
    log.error("IPC error [{}]: {}", e.getCode(), e.getMessage());
}
```

| Exception              | Code | Trigger                                                      |
|------------------------|------|--------------------------------------------------------------|
| `AbiMismatchException` | 14   | Handshake rejected, `TACHYON_MSG_ALIGNMENT` mismatch         |
| `BufferFullException`  | 9    | Ring buffer full; treat as a back-off signal on the TX path  |
| `PeerDeadException`    | -1   | Bus entered `TACHYON_STATE_FATAL_ERROR`; close immediately   |
| `TachyonException`     | -    | Base class for all native errors; carries the raw error code |

## Thread safety

`TachyonBus` is not thread-safe. Each direction (TX or RX) must be used by **at most one thread at a time**. Tachyon
is SPSC, not MPSC.

Blocking calls (`listen`, `acquireRx`, `drainBatch`) park the calling thread for their duration. Run them on a dedicated
`Thread.ofPlatform()`. Do not call blocking Tachyon methods on a virtual thread that holds a monitor
(`synchronized`), the carrier thread will pin for the full duration of the blocking call.

## NUMA binding

```java
var bus = TachyonBus.listen(path, 1 << 16);
bus.setNumaNode(0); // bind SHM pages to NUMA node 0, before the first message
```

`setNumaNode` uses `MPOL_PREFERRED + MPOL_MF_MOVE`. Call it immediately after `listen()` or `connect()`. No-op on
macOS.

## Benchmark results

Measured on Linux 6.19, Intel Core i7-12650H, 64 GiB DDR5-5600 SODIMM, JDK 21.0.10.
Producer and consumer on separate physical cores, no CPU pinning.

| Benchmark              | Score | Unit  |
|------------------------|-------|-------|
| Throughput (acquireTx) | ~76   | ns/op |
| RTT ping-pong p50      | ~739  | ns    |

JMH command:

```bash
./gradlew jmh
```

The ~76 ns/op figure covers a full `acquireTx → commitUnflushed → flush → acquireRx → commit` cycle. Panama FFM
`MethodHandle.invokeExact` dispatch overhead is ~15–20 ns per crossing; batching 64 messages reduces per-message FFM
overhead by x64.

## Type ID encoding

`typeId` is an `int` (uint32) split into two 16-bit halves since v0.4.0:

```
bits [31:16]: routeId: reserved for RPC, must be 0 for now
bits [15:0]:  msgType: application-defined discriminator
```

`routeId = 0` exactly preserves v0.3.x semantics: `TypeId.of(0, 42) == 42`.

```java
import dev.tachyon_ipc.TypeId;

// encode
int id = TypeId.of(0, 42); // == 42, identical to v0.3.x

// decode
int route = TypeId.routeId(id); // 0
int mt    = TypeId.msgType(id); // 42

// send and receive
tx.commit(size, TypeId.of(0, 42));

try (var rx = bus.acquireRx(10_000)) {
    int route = TypeId.routeId(rx.getTypeId()); // 0
    int mt = TypeId.msgType(rx.getTypeId()); // 42
}
```

`routeId >= 1` is reserved for RPC. Do not use it on consumers.

## Limitations

**Java 21+ required.** Panama FFM graduated to GA in JDK 21. Earlier versions exposed it under `--enable-preview`
with an incompatible API surface.

**`--enable-native-access=ALL-UNNAMED` is mandatory.** Without this flag, `MethodHandle` lookup into native memory
throws `IllegalCallerException` at startup.

**Linux is the primary platform.** `setNumaNode`, futex-based sleep, and `memfd_create` are Linux-specific. macOS is
supported at tier-2 with degraded wait semantics.

**SPSC only.** One producer, one consumer. For fan-in workloads, use N independent buses.

**No peer crash detection.** If the remote process crashes, blocking calls stall indefinitely.

## License

Apache 2.0
