# Core

The C++23 core of Tachyon. All language bindings compile this directory directly. There is no installed library, no
dynamic linking required at build time.

---

- [Source layout](#source-layout)
- [Memory layout](#memory-layout)
- [Ring buffer mechanics](#ring-buffer-mechanics)
- [Wait strategy](#wait-strategy)
- [C API](#c-api)
- [Building directly](#building-directly)

---

## Source layout

```text
core/
├── include/
│ ├── tachyon.h C ABI: the only header language bindings include
│ ├── tachyon.hpp C++ utilities: cpu_relax, rdtsc, tachyon_start_lifetime_as
│ └── tachyon/
│ ├── arena.hpp SPSC ring buffer: producer and consumer state
│ ├── shm.hpp Shared memory RAII wrapper (memfd / shm_open)
│ └── transport.hpp UDS handshake types and free functions
└── src/
├── arena.cpp Ring buffer implementation
├── shm.cpp memfd_create (Linux) / shm_open (macOS) + mmap
├── tachyon_c.cpp tachyon_bus_t + C API implementation
├── tachyon_rpc.cpp  RPC API implementation
└── transport_uds.cpp SCM_RIGHTS handshake over UNIX domain socket
```

## Memory layout

A single `memfd` region holds everything. Layout from offset 0:

```text
┌──────────────────────────────────────────────────────────────┐ offset 0
│ ArenaHeader   (128 bytes, alignas(128))                      │
│ magic         uint32 0x54414348 ("TACH")                     │
│ version       uint32 0x02                                    │
│ capacity      uint32 ring size in bytes                      │
│ msg_alignment uint32 TACHYON_MSG_ALIGNMENT (64)              │
│ state         atomic<BusState>                               │
├──────────────────────────────────────────────────────────────┤
│ SPSCIndices                                                  │
│ head atomic<size_t>  (alignas(128))                          │ producer-owned
│ tail atomic<size_t>  (alignas(128))                          │ consumer-owned
│ consumer_sleeping    atomic<uint32>  (alignas(128))          │ futex word
│ producer_heartbeat   atomic<uint64>  (alignas(128))          │ rdtsc stamp
│ consumer_heartbeat   atomic<uint64>  (alignas(128))          │ rdtsc stamp
├──────────────────────────────────────────────────────────────┤
│ Ring buffer data     (capacity bytes, power of two)          │
│  [ MessageHeader | payload ] [ MessageHeader | payload ] ... │
└──────────────────────────────────────────────────────────────┘
```

`ArenaHeader` and each `SPSCIndices` field occupy their own 128-byte cache line. Producer writes `head`, consumer writes
`tail`. There is no shared writes between the two sides, false sharing is physically impossible.

### MessageHeader

Every message is prefixed by a `MessageHeader` aligned to `TACHYON_MSG_ALIGNMENT` (64 bytes):

```

┌────────────────────────────────────────────────────────────┐
│ size           uint32 actual payload bytes written         │
│ type_id        uint32 application-defined discriminator    │
│ reserved_size  uint32 total slot size including header     │
│ padding        [52]u8 fills to TACHYON_MSG_ALIGNMENT       │
└────────────────────────────────────────────────────────────┘

```

`reserved_size` is always a multiple of `TACHYON_MSG_ALIGNMENT`. At `acquire_rx`, the consumer
validates all three invariants before exposing the payload pointer:

| Condition                                          | Result                 |
|----------------------------------------------------|------------------------|
| `reserved_size < sizeof(MessageHeader)` (64)       | `BusState::FatalError` |
| `reserved_size > capacity`                         | `BusState::FatalError` |
| `reserved_size & (TACHYON_MSG_ALIGNMENT - 1) != 0` | `BusState::FatalError` |
| `size > reserved_size - sizeof(MessageHeader)`     | `BusState::FatalError` |

`FatalError` is permanent and unrecoverable. Destroy the bus immediately on `PeerDeadError`.

### Skip marker

When a message does not fit in the space remaining before the ring wraps, the producer writes a `MessageHeader` with
`size = 0xFFFFFFFF` (`SKIP_MARKER`) and advances `head` to the start of the ring. The consumer detects the marker and
mirrors the wrap. The skip slot is never exposed to the caller.

---

## Ring buffer mechanics

### Producer path

```text
acquire_tx(max_size)
-> compute aligned_size = ceil((sizeof(Header) + max_size) / 64) * 64
-> check space: local_head - cached_tail + aligned_size <= capacity
-> if not: reload tail from SHM, recheck, return nullptr if still full
-> if wrap needed: write SKIP_MARKER, advance local_head
-> return pointer to payload region (local_head + sizeof(Header))

commit_tx(actual_size, type_id)
-> write PackedMeta{actual_size, type_id, aligned_size} at local_head
-> advance local_head by aligned_size
-> increment pending_tx
-> if pending_tx >= BATCH_SIZE (32): publish head to SHM, wake consumer if sleeping

flush_tx()
-> if pending_tx > 0: publish head to SHM, wake consumer if sleeping
```

`head` is only written to shared memory every 32 messages or on an explicit `flush_tx`. This
amortizes the `memory_order_release` store and the cache line invalidation across the batch.

### Consumer path

```text
acquire_rx()
-> if cached_head <= local_tail: reload head from SHM
-> still empty: return nullptr
-> read PackedMeta at local_tail (handle SKIP_MARKER wrap)
-> validate all four invariants → FatalError on failure
-> return pointer to payload region

commit_rx()
-> advance local_tail by reserved_size
-> increment pending_rx
-> if pending_rx >= BATCH_SIZE (32): publish tail to SHM

flush() / commit_rx_batch()
-> publish tail to SHM unconditionally
```

The consumer's `tail` is published back to the producer so that `acquire_tx` can reclaim slots.
Amortizing the tail writes to every 32 messages keeps the producer's `cached_tail` reload rare.

---

## Wait strategy

The consumer uses a three-phase hybrid strategy controlled by `consumer_sleeping`:

| Value | Constant             | Meaning                                   |
|-------|----------------------|-------------------------------------------|
| `0`   | `CONSUMER_AWAKE`     | Consumer is spinning                      |
| `1`   | `CONSUMER_SLEEPING`  | Consumer is parked in futex / ulock       |
| `2`   | `CONSUMER_PURE_SPIN` | Consumer will never sleep (set by caller) |

**Spin phase.** Consumer calls `cpu_relax()` (`pause` on x86, `yield` on ARM) up to `spin_threshold` times. Default is
10 000, configurable per call.

**Sleep phase.** Consumer stores `CONSUMER_SLEEPING`, issues a `seq_cst` fence, then re-reads the ring. If still empty,
it calls `SYS_futex` (Linux) or `__ulock_wait` (macOS) with a 200 ms timeout.

**Lost-wakeup window.** The `seq_cst` fence between the store and the re-read closes the race where the producer
publishes between the consumer's empty check and the sleep store. If the producer wrote in that window, the re-read
catches it before the syscall.

**Producer side.** On `flush_tx`, if `consumer_sleeping != CONSUMER_PURE_SPIN`, the producer issues a `seq_cst` fence
and re-checks the word. If it reads `CONSUMER_SLEEPING`, it calls `FUTEX_WAKE`. In pure-spin mode, the fence and the
load are skipped entirely. This is the `tachyon_bus_set_polling_mode` optimization.

**TSan.** `__tsan_acquire` / `__tsan_release` annotations wrap `platform_wait` / `platform_wake` so that TSan sees the
correct happens-before edge through the futex boundary.

---

## C API

`tachyon.h` is the only public header.
All symbols are exported under `TACHYON_ABI` (`__attribute__((visibility("default")))`). Everything else is compiled
with `-fvisibility=hidden`.

### Lifecycle

```c++
tachyon_error_t tachyon_bus_listen(const char *socket_path, size_t capacity, tachyon_bus_t **out);
tachyon_error_t tachyon_bus_connect(const char *socket_path, tachyon_bus_t **out);
void            tachyon_bus_destroy(tachyon_bus_t *bus);
void            tachyon_bus_ref(tachyon_bus_t *bus);
```

`tachyon_bus_t` is reference-counted (`std::atomic<uint32_t>`). `listen` and `connect` set the initial count to 1.
`tachyon_bus_ref` increments it. `tachyon_bus_destroy` decrements and deletes when it reaches 0.

`tachyon_bus_listen` blocks on `accept`. It releases after `sendmsg` completes and the socket file is unlinked. See [
`INTEGRATION.md`](../INTEGRATION.md) for the full socket lifecycle.

### TX path

```c++
void           *tachyon_acquire_tx(tachyon_bus_t *bus, size_t max_payload_size);
tachyon_error_t tachyon_commit_tx(tachyon_bus_t *bus, size_t actual_payload_size, uint32_t type_id);
tachyon_error_t tachyon_rollback_tx(tachyon_bus_t *bus);
void            tachyon_flush(tachyon_bus_t *bus);
```

`acquire_tx` returns a pointer into the ring buffer. Write up to `max_payload_size` bytes into it, then call `commit_tx`
with the actual bytes written. `commit_tx` does not flush, call `tachyon_flush` after the last message in a batch to
make all committed messages visible to the consumer.

`rollback_tx` rewinds `local_head` to its pre-acquire position. The slot is returned to the ring as if the acquire never
happened. No message is published.

### RX path

```c++
const void     *tachyon_acquire_rx_blocking(tachyon_bus_t *bus, uint32_t *out_type_id,
                                             size_t *out_actual_size, uint32_t spin_threshold);
tachyon_error_t tachyon_commit_rx(tachyon_bus_t *bus);

size_t          tachyon_drain_batch(tachyon_bus_t *bus, tachyon_msg_view_t *out_views,
                                     size_t max_msgs, uint32_t spin_threshold);
tachyon_error_t tachyon_commit_rx_batch(tachyon_bus_t *bus,
                                         const tachyon_msg_view_t *views, size_t count);
```

`acquire_rx_blocking` returns `nullptr` on `EINTR` from the futex. The caller must check `tachyon_get_state` before
retrying. `nullptr` on `FatalError` is a permanent failure.

`tachyon_msg_view_t` is a 32-byte struct (layout-compatible with `RxView`):

```c++
typedef struct {
    const void *ptr;         // pointer into SHM ring - valid until commit_rx_batch
    size_t      actual_size;
    size_t      reserved_;   // internal - do not read
    uint32_t    type_id;
    uint32_t    padding_;
} tachyon_msg_view_t;
```

### Configuration

```c++
void            tachyon_bus_set_polling_mode(tachyon_bus_t *bus, int pure_spin);
tachyon_error_t tachyon_bus_set_numa_node(const tachyon_bus_t *bus, int node_id);
tachyon_state_t tachyon_get_state(const tachyon_bus_t *bus);
```

`set_polling_mode(bus, 1)` stores `CONSUMER_PURE_SPIN` into `consumer_sleeping`. Call it on both the producer and the
consumer immediately after handshake, before the first message. If only the consumer calls it, the producer still checks
`consumer_sleeping` on every flush and finds `CONSUMER_PURE_SPIN`, the fence and the wake are skipped. Calling it on
both sides is the correct pattern.

`set_numa_node` calls `SYS_mbind` with `MPOL_PREFERRED | MPOL_MF_MOVE`. No-op on macOS.

---

## Building directly

The core is not installed as a shared library. Language bindings vendor the sources and compile them alongside their own
native code. The `tachyon` CMake target builds a shared library for testing only.

```bash
# Shared library + tests (Clang, Release)
cmake --preset clang-release
cmake --build --preset clang-release
ctest --test-dir build/clang-release/test --output-on-failure

# With sanitizers
cmake --preset asan   && cmake --build --preset asan
cmake --preset tsan   && cmake --build --preset tsan
cmake --preset msan   && cmake --build --preset msan   # requires ci/build_msan_libcxx.sh first
```

For wire protocol and ABI guarantees → [`ABI.md`](../ABI.md).  
For socket lifecycle and supervision patterns → [`INTEGRATION.md`](../INTEGRATION.md).
