# Integration Guide

## Socket lifecycle

The UDS socket is a **one-shot bootstrap mechanism**, not a persistent connection. `tachyon_bus_listen()` creates the
socket file, waits for exactly one `accept()`, sends the `memfd` file descriptor via `SCM_RIGHTS`, then closes both the
client and listening socket descriptors, and **immediately unlinks the socket file**. After `tachyon_bus_connect()`
returns, nobody is listening on the socket path and the file no longer exists; the entire IPC path runs through shared
memory.

Consequences:

- The socket path is free immediately after the handshake completes. A second `listen()` on the same path can `bind`
  without any prior `unlink`, there is nothing to clean up.
- A second `connect()` to the same path after the handshake will get `ENOENT`, the file has already been removed.
- Deleting the socket file manually has no effect on an established bus.
- During a crash **before** the handshake completes (i.e., between `bind` and `sendmsg`), the socket file survives.
  Clean it up before restarting the listener on the same path: `rm -f /tmp/your.sock` or `os.unlink()`. If the crash
  occurs **after** the handshake, the file is already gone.

### Relisten pattern

To accept a new producer after the previous one disconnects, destroy the existing bus and call `listen()` again on the
same path. Each `listen()` call creates a new SHM arena, ring buffer state does not persist across sessions.

Python:

```python
while True:
	try:
		with tachyon.Bus.listen(SOCKET_PATH, CAPACITY) as bus:
			for msg in bus:
				process(msg)
	except tachyon.PeerDeadError:
		log.warning("producer died, relistening")
		time.sleep(0.1)
```

Rust:

```rust
loop {
    let bus = Bus::listen(SOCKET_PATH, CAPACITY) ?;
    loop {
        match bus.acquire_rx(10_000) {
          Ok(guard) => { process(guard.data()); guard.commit() ?; }
          Err(TachyonError::PeerDead) => break,
          Err(e) => return Err(e),
        }
    }
}
```

C++:

```c++
for (;;) {
    tachyon_bus_t *bus = nullptr;
    tachyon_bus_listen(SOCKET_PATH, CAPACITY, &bus);

    uint32_t type_id = 0; size_t sz = 0;
    while (true) {
        const void *ptr = tachyon_acquire_rx_blocking(bus, &type_id, &sz, 10000);
        if (ptr == nullptr) {
            // nullptr has two causes EINTR (signal) or FatalError.
            if (tachyon_get_state(bus) == TACHYON_STATE_FATAL_ERROR) break;
            continue; // EINTR - retry
        }
        process(ptr, sz);
        tachyon_commit_rx(bus);
    }

    tachyon_bus_destroy(bus);
}
```

---

## Supervision

### PeerDeadError

`PeerDeadError` (Python) / `TachyonError::PeerDead` (Rust) / `TACHYON_STATE_FATAL_ERROR` (C++) is raised when the bus
transitions to `FatalError`.

**The only conditions that trigger `FatalError`:** a corrupted message header is detected in `acquire_rx()`,
specifically when any of the following holds:

- `reserved_size < sizeof(MessageHeader)` (64 bytes)
- `reserved_size > capacity`
- `reserved_size` is not a multiple of `TACHYON_MSG_ALIGNMENT` (64)
- `size > reserved_size - sizeof(MessageHeader)`

This indicates the ring buffer has been written with an incompatible layout or corrupted externally.

The 200 ms futex timeout is a **wait bound**, not a dead-peer detector. When the consumer sleeps waiting for a message
and the futex times out, it resets its spin counter and retries, it does not transition to `FatalError`. An idle
producer is not a dead producer.

Conditions that trigger `PeerDeadError`:

- The ring buffer contains a message header with invalid `reserved_size` or `size` fields, typically caused by a
  producer compiled with a different `TACHYON_MSG_ALIGNMENT` value that bypassed the handshake check, or by external
  memory corruption.

Conditions that do **not** trigger `PeerDeadError`:

- Producer process crashed: the consumer blocks indefinitely waiting for the next message. Use an external supervisor
  or OS-level process monitoring to detect this.
- Producer is slow, idle, or temporarily suspended: the consumer sleeps via futex and wakes on the next message
  regardless of elapsed time.
- Producer has not written anything for several seconds: the 200 ms timeout is a spin bound, not a liveness deadline.

**Important:** Tachyon does not detect producer crashes. If dead-peer detection is required, use an external heartbeat
mechanism (e.g., a dedicated health check bus, a shared atomic counter incremented by the producer, or OS process
monitoring via `pidfd` or `SIGCHLD`).

### Restart pattern

The recommended supervisor loop keeps the listener alive indefinitely and respawns the producer side from a process
manager (systemd, supervisord, custom watchdog).

```
┌─────────────────────────────────┐
│  Supervisor                     │
│                                 │
│  loop:                          │
│    start producer process       │
│    wait for exit / SIGKILL      │
│    sleep backoff                │
└─────────────────────────────────┘
          │ connects to
          ▼
┌─────────────────────────────────┐
│  Consumer (long-lived)          │
│                                 │
│  loop:                          │
│    Bus.listen(path, capacity)   │  ← blocks until producer connects
│    drain messages               │
│    on PeerDeadError → continue  │  ← loop back to listen()
└─────────────────────────────────┘
```

The consumer owns the socket. It calls `listen()` in a loop. The producer connects, sends, and exits (or crashes). The
consumer detects the dead peer, destroys the bus, and calls `listen()` again. The socket is recreated on each `listen()`
call, the producer does not need to do anything special.

### NUMA binding

If producer and consumer are on different NUMA nodes, all ring buffer accesses cross the interconnect. Call
`set_numa_node()` immediately after `listen()` or `connect()` to migrate the SHM pages before the hot path begins.

```python
with tachyon.Bus.listen(path, capacity) as bus:
	bus.set_numa_node(0)  # pin to node 0 - call before first message
	for msg in bus:
		...
```

```rust
let bus = Bus::listen(path, capacity) ?;
bus.set_numa_node(0) ?;   // MPOL_PREFERRED + MPOL_MF_MOVE
```

### Pure-spin mode

If the consumer runs in a dedicated thread that never parks (e.g. a `SCHED_FIFO` reflector or a benchmark), call
`tachyon_bus_set_polling_mode` immediately after handshake. This sets `consumer_sleeping` to `CONSUMER_PURE_SPIN`, which
causes the producer to skip the `atomic_thread_fence(seq_cst)` + `consumer_sleeping` load on every `flush_tx`. Do not
call this if the consumer thread may sleep or yield, the producer will never issue a futex wake, and the consumer will
spin indefinitely rather than sleeping.

```c++
tachyon_bus_set_polling_mode(rx, 1); // consumer: I will never sleep
tachyon_bus_set_polling_mode(tx, 1); // producer: skip wake check
```

### Heartbeat granularity

`tachyon-top` displays `producer_hb_age_us` and `consumer_hb_age_us`, the age of the last observed producer and
consumer heartbeat. These counters are updated at batch boundaries only (every 32 committed messages on the producer
side, every 32 committed messages on the consumer side). They are not updated on every flush.

Consequences:

- On a low-throughput bus (fewer than 32 messages per burst), heartbeat age grows continuously and does not reflect
  actual liveness. This is expected.
- On a high-throughput bus, heartbeat age stays bounded at the batch amortization interval.
- Heartbeat values are observable only via `tachyon-top` (external `mmap(PROT_READ)`). They have no effect on IPC
  correctness, ordering, or latency.

Do not use heartbeat age as a dead-peer detector. See the supervision section above for reliable peer monitoring
strategies.

### Syscall containment

Tachyon's post-handshake hot path emits a single syscall type (`futex` on Linux, `__ulock` on macOS). If your deployment
requires seccomp-BPF containment, apply the filter after `tachyon_bus_listen()`/`tachyon_bus_connect()` returns.
Pre-built profiles are available in `contrib/seccomp/`. Do not apply them from within a polyglot runtime (Go, Python,
Java, Node.js).

---

## RPC supervision

### Correlation ID lifecycle

Each `tachyon_rpc_call` assigns a monotonically increasing `correlation_id` starting at 1. The callee echoes it verbatim
in the reply via `tachyon_rpc_commit_reply`. `tachyon_rpc_wait` blocks until it observes the expected ID in `arena_rev`.
A mismatch (callee sent a reply with the wrong ID) transitions `arena_rev` to `FatalError` and returns `nullptr`. The
bus must be destroyed immediately.

Valid range: `[1, UINT64_MAX]`. `correlation_id = 0` is rejected by `tachyon_rpc_commit_call` with
`TACHYON_ERR_INVALID_SZ`.

### Timeout strategy

`tachyon_rpc_wait` has no wall-clock timeout. If the callee crashes after receiving a request but before sending a
reply, the caller blocks indefinitely. This is the same contract as the SPSC hot path: Tachyon does not detect peer
crashes.

Recommended pattern: run the caller with a deadline on the application side. If the deadline fires, destroy the bus and
reconnect.

```c++
// Caller with external deadline
auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

uint64_t cid = 0;
tachyon_rpc_call(rpc, payload, size, msg_type, &cid);

// Poll with non-blocking spin threshold, check deadline between retries
const void *ptr = nullptr;
while (!ptr) {
    if (std::chrono::steady_clock::now() > deadline) {
        tachyon_rpc_destroy(rpc);
        return handle_timeout();
    }
    uint32_t msg_type_out = 0;
    size_t actual_size = 0;
    ptr = tachyon_rpc_wait(rpc, cid, &actual_size, &msg_type_out, 1000);
}
```

### Dead-callee detection

If the callee process exits after `tachyon_rpc_listen` returns but before processing a request, `arena_fwd` retains the
message indefinitely. The caller's `tachyon_rpc_wait` blocks. Detect this externally:

- `pidfd_open` + `poll` on the callee PID.
- A dedicated health check SPSC bus with a heartbeat message sent by the callee on each request iteration.
- OS process monitoring via `SIGCHLD` in the supervisor.

### Relisten pattern (callee side)

```c++
for (;;) {
    tachyon_rpc_bus_t *rpc = nullptr;
    tachyon_rpc_listen(SOCKET_PATH, CAP_FWD, CAP_REV, &rpc);

    for (;;) {
        uint64_t cid = 0; uint32_t msg_type = 0; size_t sz = 0;
        const void *ptr = tachyon_rpc_serve(rpc, &cid, &msg_type, &sz, 10000);
        if (!ptr) {
            if (tachyon_rpc_get_state(rpc) == TACHYON_STATE_FATAL_ERROR) break;
            continue; // EINTR
        }
        process(ptr, sz);
        tachyon_rpc_commit_serve(rpc);

        void *reply_slot = tachyon_rpc_acquire_reply_tx(rpc, reply_size);
        // write reply...
        tachyon_rpc_commit_reply(rpc, cid, reply_size, reply_type);
    }

    tachyon_rpc_destroy(rpc);
}
```

### `serve` before `reply` ordering

`tachyon_rpc_serve` acquires a slot in `arena_fwd`. `tachyon_rpc_acquire_reply_tx` acquires a slot in `arena_rev`.
Holding both simultaneously is safe but wastes one arena slot for the duration of processing. Call
`tachyon_rpc_commit_serve` before `tachyon_rpc_acquire_reply_tx` to release the request slot first. On a small
`cap_fwd`, failing to do so can deadlock if the caller queues another request while the callee holds the slot.

---

## Type ID encoding

Every message carries a `uint32_t type_id` field. As of v0.4.0, this field is split into two 16-bit halves by
convention:

```
bits [31:16]  route_id   routing discriminator, reserved for RPC
bits [15:0]   msg_type   application-defined message type
```

The wire layout of `MessageHeader` is unchanged. The split is a semantic convention, not a structural change.

### route_id = 0

`route_id = 0` is reserved for direct SPSC usage without RPC. All v0.3.x `type_id` values fall in this range. A
`type_id` of `42` is identical to `TACHYON_TYPE_ID(0, 42)`.

### route_id >= 1

Values in bits [31:16] other than zero are reserved for the RPC primitive introduced in v0.5.0. Do not use them in
v0.4.0 consumers. Behavior is undefined.

### Macro helpers

C++:

```c++
#include <tachyon.h>

uint32_t id = TACHYON_TYPE_ID(0, 42);  // encode
uint16_t route = TACHYON_ROUTE_ID(id); // 0
uint16_t msg = TACHYON_MSG_TYPE(id);   // 42
```

Equivalent helpers are available in every binding under the same names (`make_type_id`, `route_id`, `msg_type` in
Python, Rust, Go, Node.js; `TypeId.of`, `TypeId.routeId`, `TypeId.msgType` in Java and Kotlin). See
[`MIGRATION.md`](./MIGRATION.md) for per-language snippets.

### Sentinel values

`type_id = 0` (`TACHYON_TYPE_ID(0, 0)`) is conventionally used as a shutdown sentinel in the cross-language examples.
This convention is unchanged. `route_id = 0` and `msg_type = 0` together still read as `type_id == 0`.

---

## Capacity sizing

The ring buffer must be large enough to absorb producer bursts during consumer pauses (batch compute, GC, scheduling
preemption).

### Formula

```
CAPACITY >= max_burst_messages * aligned_message_size
```

Where `aligned_message_size` is:

```
aligned = ceil((sizeof(MessageHeader) + payload_bytes) / 64) × 64
```

`sizeof(MessageHeader)` is 64 bytes (`TACHYON_MSG_ALIGNMENT`). Capacity must be a **power of two**.

### Examples

| Use case                          | Payload      | Max burst   | Aligned slot | Minimum capacity   |
|-----------------------------------|--------------|-------------|--------------|--------------------|
| Market ticks                      | 32 bytes     | 10 000 msgs | 128 bytes    | 2 MB (`1 << 21`)   |
| ML feature vectors                | 1 024 bytes  | 512 frames  | 1 088 bytes  | 1 MB (`1 << 20`)   |
| Audio frames (1024 samples × f32) | 4 096 bytes  | 64 frames   | 4 160 bytes  | 512 KB (`1 << 19`) |
| Large blobs                       | 65 536 bytes | 8 msgs      | 65 600 bytes | 1 MB (`1 << 20`)   |

Round up to the next power of two and add a 2× safety margin for bursty producers.

### Practical defaults

```python
# Market data - low latency, small payload
CAPACITY = 1 << 20  # 1 MB

# ML inference - larger payload, Python consumer pause during matmul
CAPACITY = 1 << 23  # 8 MB

# Audio / video pipeline - large frames, real-time consumer
CAPACITY = 1 << 22  # 4 MB
```

If the producer returns `TachyonError` / `TACHYON_ERR_FULL`, the buffer is too small for the burst rate, increase
capacity or reduce burst size. The anti-overwrite shield never drops messages silently; the producer blocks or returns
an error instead.

### Browser WASM

`size_t` is 32-bit on wasm32, so the core rejects any capacity above `INT32_MAX`. Combined with the power-of-two rule,
the largest usable ring is `1 << 30` (1 GiB).

Sizing is otherwise the same formula, but the budget is not: rings live in the module's linear memory alongside
everything else the page allocates. Treat the browser as a memory-constrained target and size for the burst, not for
headroom.

### Memory cost

Tachyon uses `memfd_create` + `mmap(MAP_POPULATE)`, which allocates physical pages at `listen()` time.
`CAPACITY = 1 << 23` (8 MB) costs 8 MB of RAM in the producer process and 8 MB in the consumer process (two `mmap`
mappings of the same `memfd`). The physical pages are shared, total RAM cost is 8 MB, not 16.

On wasm32 there is no `memfd` and no second mapping: the ring is a single `aligned_alloc` in the module's linear memory,
so the cost is the capacity, once.

---

## Star Bus

The `StarBus` (`tachyon_star_t`) aggregates N independent SPSC rings under a single round-robin polling loop. One
consumer process owns the `StarBus`; each producer (spoke) owns one `tachyon_bus_t` on the listener side. The hub holds
one connector-side `tachyon_bus_t` per spoke.

### Handshake and topology

Each spoke is a fully independent SPSC bus. The hub calls `tachyon_bus_connect()` for each spoke, then passes all
connector handles to `tachyon_star_create()`. The star ref-counts each bus internally; the caller may destroy its
handles immediately after `create()` returns.

Socket lifecycle is identical to the single-bus case: each path is unlinked after the handshake, and the socket file no
longer exists once the hub has connected. The hot path runs entirely through shared memory.

### Hub-and-spoke pattern

```c++
// Spokes start first and call tachyon_bus_listen.
// Hub connects to all spokes, then creates the star.
tachyon_bus_t *buses[N];
for (size_t i = 0; i < N; ++i)
    tachyon_bus_connect(spoke_path(i), &buses[i]);

tachyon_star_t *star = nullptr;
tachyon_star_create(buses, N, nullptr, &star);

// Hub owns the hot path.
static constexpr size_t MAX_BATCH = N * 32;
tachyon_msg_view_t views[MAX_BATCH];
size_t             spoke_indices[MAX_BATCH];

for (;;) {
    const size_t count = tachyon_star_poll(star, views, MAX_BATCH, /*budget_us=*/5000, spoke_indices);
    for (size_t i = 0; i < count; ++i) {
        process(views[i].ptr, views[i].actual_size, spoke_indices[i]);
    }

    if (count > 0) {
        tachyon_star_commit(star);
    }
}
```

`tachyon_star_poll` drains messages from all N spokes in a single call, round-robining until either `max_total` messages
have been collected or the TSC-bounded `budget_us` has elapsed. `tachyon_star_commit` advances the consumer tail for
every spoke that contributed messages; it uses the internal `pending_` state accumulated during the poll. The view's
array is not passed back to `commit`.

### Budget tuning

`budget_us` is a TSC-bounded wall-clock budget. The poll loop exits when `rdtsc() >= deadline` even if messages remain.
Choose it based on the end-to-end latency budget for your application tier.

| Regime                                  | Recommended `budget_us` | Notes                                         |
|-----------------------------------------|-------------------------|-----------------------------------------------|
| Ultra-low latency (HFT, realtime audio) | 1 to 10                 | Tight deadline; relies on pure-spin mode      |
| Market data aggregation                 | 50 to 500               | Balance throughput vs. per-message latency    |
| General fan-in, batch analytics         | 1000 to 10000           | Maximise batch size; amortise commit overhead |

If the budget expires before any message arrives, `poll()` returns 0 and `commit()` is a no-op. Calling `commit()` on an
empty poll is safe.

A budget that is too short under load causes `poll()` to return after draining fewer spokes than intended. Increase
`budget_us` or call `poll()` in a tight loop without sleeping.

### Pure-spin mode

Call `tachyon_bus_set_polling_mode(bus[i], 1)` on each connector-side handle before passing it to
`tachyon_star_create()`. This tells each producer that the consumer (hub) will never sleep, eliminating the
`atomic_thread_fence(seq_cst)` and `consumer_sleeping` load on every producer flush. Only enable this when the hub
thread is dedicated and never yields.

```c++
for (size_t i = 0; i < N; ++i) {
    tachyon_bus_connect(spoke_path(i), &buses[i]);
    tachyon_bus_set_polling_mode(buses[i], 1);
}
tachyon_star_create(buses, N, nullptr, &star);
```

### NUMA binding across nodes

When producers and the hub run on different NUMA nodes, all ring buffer accesses cross the interconnect.
`tachyon_star_create()` accepts an optional `node_ids` array (one entry per spoke) to bind each spoke's SHM pages to the
requested NUMA node immediately after the star is created.

```c++
// Spoke 0 and 1 producers are on node 0; spoke 2 is on node 1.
const int node_ids[] = {0, 0, 1};
tachyon_star_create(buses, 3, node_ids, &star);
```

Negative values skip binding for that spoke. `nullptr` disables NUMA binding entirely and is appropriate when all
processes are confined to a single node.

Call `tachyon_bus_set_numa_node()` directly on individual connector handles before `tachyon_star_create()` if you need
finer control (for example, binding only some spokes before the star is assembled).

### FatalError isolation per spoke

Each spoke is an independent SPSC ring with its own state machine. A `TACHYON_STATE_FATAL_ERROR` on one spoke does not
affect the others.

```c++
for (size_t i = 0; i < count; ++i) {
    const size_t spoke = spoke_indices[i];

    if (tachyon_star_get_state(star, spoke) == TACHYON_STATE_FATAL_ERROR) {
        handle_fatal(spoke);
        continue;
    }

    process(views[i].ptr, views[i].actual_size, spoke);
}
tachyon_star_commit(star);
```

`tachyon_star_get_state(star, spoke_idx)` reads the atomic state of the underlying connector arena for that spoke. The
check is advisory; messages already returned by `poll()` remain accessible until `commit()` is called.

### Supervisor loop

The star does not detect producer crashes (same contract as SPSC). If a spoke producer exits cleanly or crashes, the
hub's `poll()` simply never receives another message from that spoke. Detect this externally via `pidfd`, `SIGCHLD`, or
a dedicated heartbeat SPSC bus.

To accept a new producer on a spoke that has gone silent, destroy the star, destroy the affected bus, call
`tachyon_bus_connect()` on the new listener, and recreate the star with the replacement handle. There is no
`replace_spoke` operation; the star is immutable after creation.

```c++
// Replace spoke 2 after its producer was restarted.
tachyon_star_destroy(star);
tachyon_bus_destroy(buses[2]);

tachyon_bus_connect(spoke_path(2), &buses[2]);
tachyon_bus_set_polling_mode(buses[2], 1);
tachyon_star_create(buses, N, node_ids, &star);
```

### Capacity sizing for multi-spoke rings

Use the same formula as the single-bus case per spoke. The hub must commit before the slowest spoke's ring overflows.
With N spokes each sending at rate R msgs/s and a poll budget of B microseconds, the minimum per-spoke capacity is:

```
CAPACITY_per_spoke >= R * (B / 1_000_000) * aligned_message_size * safety_margin
```

A safety margin of 4x is recommended for bursty producers. Spokes do not share memory; each ring is independent and
sized independently.
