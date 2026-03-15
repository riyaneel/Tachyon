# Tachyon

---

Tachyon is a bare-metal, lock-free Inter-Process Communication (IPC) primitive. It implements a strictly bounded,
Single-Producer Single-Consumer (SPSC) ring buffer operating directly over POSIX shared memory.

Engineered for extreme throughput and deterministic sub-microsecond latency, Tachyon bypasses the kernel, network
stacks, and serialization schemas entirely. It acts as a pure, localized memory pipe, moving raw byte payloads across
language boundaries at the physical limit of the CPU's memory controller.

---

## 1. System Architecture

Tachyon achieves deterministic latency on the hot path by strictly decoupling connection lifecycle management (the
Control Plane) from data transmission (the Data Plane).

### 1.1 The Control Plane (Bootstrapping & Discovery)

Process discovery and connection handshakes are negotiated over Unix Domain Sockets (UDS). The UDS is exclusively used
for two operations:

1. **ABI Handshake:** Exchanging a strictly versioned payload containing ring capacity and memory layout configurations.
2. **File Descriptor Passing:** Transferring anonymous shared memory file descriptors to the peer via the `SCM_RIGHTS`
   ancillary message.

**Non-Blocking Initialization:** To ensure host applications remain responsive, the UDS `accept()` and `connect()`
phases utilize non-blocking I/O (`poll()`). This allows frameworks (e.g., CPython) to periodically release their
execution locks, check for signals (`SIGINT`), and cleanly abort bootstrapping without inducing kernel-level deadlocks.

**Runtime ABI Validation:** Tachyon dynamically validates micro-architectural parameters during the handshake. If the
producer and consumer were compiled with differing cache-line alignments (`TACHYON_MSG_ALIGNMENT`), the connection is
instantly rejected, physically preventing silent memory corruption or segmentation faults.

### 1.2 The Data Plane (Hot Path)

Once the memory mapping (`mmap`) is established, the socket is permanently discarded. All subsequent I/O operations
occur directly in the shared RAM segment. The SPSC ring buffer relies on C++26 atomic primitives utilizing strict
`memory_order_acquire` and `memory_order_release` semantics. Data materialized by the producer is instantly visible in
the consumer's virtual address space.

---

## 2. Hardware Sympathy & Concurrency

Tachyon is explicitly designed around the physical constraints of modern CPU micro-architectures (x86_64, aarch64).

### 2.1 Cache-Line Packing & False Sharing Elimination

Every internal control structure—including message headers, atomic read/write indices, and watchdog flags—is strictly
padded and aligned to 64-byte or 128-byte boundaries. This mathematical alignment guarantees the absolute elimination of
false sharing and cache-line straddling between producer and consumer cores.

### 2.2 Amortized Atomics & Vectorized Batching

To circumvent the heavy CPU pipeline penalty of memory barriers, Tachyon exposes a high-throughput batching topology (
`drain_batch`). Consumers can ingest thousands of contiguous messages in a single Foreign Function Interface (FFI)
boundary crossing. The engine computes the spatial advancement using SIMD-friendly reduction loops and updates the
shared atomic tail index with a single instruction, drastically amortizing synchronization costs.

### 2.3 The FFI Drop-Lock Pattern (MPSC/SPMC Safety)

While the underlying memory ring is SPSC, Tachyon is designed to operate safely within multi-threaded host runtimes (
e.g., Python with `Py_GIL_DISABLED` or Go runtimes). The C-API rigorously employs a **Drop-Lock** pattern: local atomic
spinlocks are acquired strictly to evaluate the ring's state and are instantly released before the thread yields to the
OS or begins a spin-cycle. This guarantees that multiple threads within the same process can concurrently contend for
the bus without starving each other or deadlocking the application.

### 2.4 Hybrid Wait Strategy & Graceful Degradation

Unbounded spin-polling is hostile to multi-tenant servers. Tachyon implements a multi-stage synchronization fallback:

1. **Spin Phase:** The consumer actively polls the L1 cache using `cpu_relax()` for a bounded threshold, ensuring
   minimal latency during dense burst traffic.
2. **Sleep Phase:** Upon breaching the threshold, the consumer gracefully yields to the OS scheduler using lightweight,
   kernel-level wait queues (`SYS_futex` on Linux, `__ulock` on macOS).
3. **Periodic Wakeup:** Kernel sleeps are bounded by timeouts, forcing the thread to periodically yield back to the host
   runtime to process background tasks and OS signals, rather than permanently blocking in kernel space.

---

## 3. Resilience & Transactional Safety

Shared memory IPC expands the failure domain of interconnected processes. Tachyon confines this blast radius through
strict transactional semantics.

### 3.1 Torn-Write Protection & Exception Rollbacks

Message headers are published to the consumer strictly after the payload has been fully materialized and fenced. In
managed language bindings (like Python), write transactions are guarded by context managers. If an unhandled exception
occurs while writing the payload, the transaction automatically rolls back by injecting a zero-sized `SKIP_MARKER`. The
ring buffer continues to advance cleanly, and the consumer silently ignores the aborted transaction.

### 3.2 Dead-Peer Detection (Watchdog)

Tachyon embeds monotonic heartbeat counters in the shared memory layout. If a sleeping consumer is abruptly terminated (
`SIGKILL`), the OS-level wait is cleanly interrupted for the producer. The engine relies on robust timeout semantics and
UDS lifecycle monitoring to detect unresponsive peers, ensuring that idle connections are not falsely flagged as dead.

---

## 4. The Zero-Copy Contract & DLPack

Tachyon ensures true zero-copy transport at the physical layer, surfacing this capability according to the host
language's memory model:

* **Tier 1 - Native (C++/Rust):** Direct raw pointer access to the memory segment. Ownership is tied to lexical scopes
  or borrow checkers, resulting in zero allocations and zero copies.
* **Tier 2 - Machine Learning (Python):** Deep integration with the Buffer Protocol (`memoryview`) and **DLPack** (
  `__dlpack__`). Inference frameworks such as PyTorch, JAX, and NumPy can ingest raw IPC payloads directly into hardware
  tensors with zero copying. Memory safety is strictly enforced via **Poison-Pill** context managers that prevent
  dangling tensors from outliving the underlying ring buffer transaction.
* **Tier 3 - Garbage Collected (Go/Node.js):** Bulk memory ingestion via the batch API, copying data into the language
  heap while heavily amortizing CGO/N-API boundary overhead.

---

## 5. Build Requirements

Tachyon leverages modern C++26 features and requires an up-to-date build toolchain.

* **CMake:** 3.31+ (Required for C++26 support)
* **Compiler:** GCC 14.0+ or Clang 17.0+
* **OS:** Linux (Kernel 5.10+) or macOS 13+

### Compilation

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## License

This project is licensed under the [Apache 2.0](./LICENSE).
