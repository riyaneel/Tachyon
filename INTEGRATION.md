# Integration Guide

## Socket lifecycle

The UDS socket is a **one-shot bootstrap mechanism**, not a persistent connection. `tachyon_bus_listen()` creates the
socket file, waits for exactly one `accept()`, sends the `memfd` file descriptor via `SCM_RIGHTS`, then closes and
unlinks the socket. After `tachyon_bus_connect()` returns, the socket file no longer exists and the entire IPC path runs
through shared memory.

Consequences:

- The socket path can be reused immediately after the consumer calls `Bus.listen()` again — there is no TIME_WAIT or
  linger state.
- A second `connect()` to the same path after the handshake will get `ECONNREFUSED` — the socket is gone.
- Deleting the socket file manually has no effect on an established bus.
- On crash, the socket file may survive. Clean it up before restarting the listener: `rm -f /tmp/your.sock` or
  `os.unlink()`.

### Relisten pattern

To accept a new producer after the previous one disconnects, destroy the existing bus and call `listen()` again on the
same path. Each `listen()` call creates a new SHM arena — ring buffer state does not persist across sessions.

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

```cpp
for (;;) {
    tachyon_bus_t *bus = nullptr;
    tachyon_bus_listen(SOCKET_PATH, CAPACITY, &bus);

    uint32_t type_id = 0; size_t sz = 0;
    while (true) {
        const void *ptr = tachyon_acquire_rx_blocking(bus, &type_id, &sz, 10000);
        if (!ptr || tachyon_get_state(bus) == TACHYON_STATE_FATAL_ERROR) break;
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

**The only condition that triggers `FatalError`:** a corrupted message header is detected in `acquire_rx()` —
specifically when `reserved_size > capacity` or `size > reserved_size - sizeof(MessageHeader)`. This indicates the
shared memory ring buffer has been written with an incompatible layout or has been corrupted externally.

The 200 ms futex timeout is a **wait bound**, not a dead-peer detector. When the consumer sleeps waiting for a message
and the futex times out, it resets its spin counter and retries — it does not transition to `FatalError`. An idle
producer is not a dead producer.

Conditions that trigger `PeerDeadError`:

- The ring buffer contains a message header with invalid `reserved_size` or `size` fields — typically caused by a
  producer compiled with a different `TACHYON_MSG_ALIGNMENT` value that bypassed the handshake check, or by external
  memory corruption.

Conditions that do **not** trigger `PeerDeadError`:

- Producer process crashed — the consumer blocks indefinitely waiting for the next message. Use an external supervisor
  or OS-level process monitoring to detect this.
- Producer is slow, idle, or temporarily suspended — the consumer sleeps via futex and wakes on the next message
  regardless of elapsed time.
- Producer has not written anything for several seconds — the 200 ms timeout is a spin bound, not a liveness deadline.

**Important:** Tachyon does not detect producer crashes. If dead-peer detection is required, use an external heartbeat
mechanism (e.g. a dedicated health-check bus, a shared atomic counter incremented by the producer, or OS process
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
call — the producer does not need to do anything special.

### NUMA binding

If producer and consumer are on different NUMA nodes, all ring buffer accesses cross the interconnect. Call
`set_numa_node()` immediately after `listen()` or `connect()` to migrate the SHM pages before the hot path begins.

```python
with tachyon.Bus.listen(path, capacity) as bus:
    bus.set_numa_node(0)  # pin to node 0 — call before first message
    for msg in bus:
        ...
```

```rust
let bus = Bus::listen(path, capacity) ?;
bus.set_numa_node(0) ?;   // MPOL_PREFERRED + MPOL_MF_MOVE
```

---

## Capacity sizing

The ring buffer must be large enough to absorb producer bursts during consumer pauses (batch compute, GC, scheduling
preemption).

### Formula

```
CAPACITY ≥ max_burst_messages × aligned_message_size
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
# Market data — low latency, small payload
CAPACITY = 1 << 20  # 1 MB

# ML inference — larger payload, Python consumer pause during matmul
CAPACITY = 1 << 23  # 8 MB

# Audio / video pipeline — large frames, real-time consumer
CAPACITY = 1 << 22  # 4 MB
```

If the producer returns `TachyonError` / `TACHYON_ERR_FULL`, the buffer is too small for the burst rate — increase
capacity or reduce burst size. The anti-overwrite shield never drops messages silently; the producer blocks or returns
an error instead.

### Memory cost

Tachyon uses `memfd_create` + `mmap(MAP_POPULATE)`, which allocates physical pages at `listen()` time.
`CAPACITY = 1 << 23` (8 MB) costs 8 MB of RAM in the producer process and 8 MB in the consumer process (two `mmap`
mappings of the same `memfd`). The physical pages are shared — total RAM cost is 8 MB, not 16.