# Integration Guide

## Socket lifecycle

The UDS socket is a **one-shot bootstrap mechanism**, not a persistent
connection. `tachyon_bus_listen()` creates the socket file, waits for exactly
one `accept()`, sends the `memfd` file descriptor via `SCM_RIGHTS`, then closes
and unlinks the socket. After `tachyon_bus_connect()` returns, the socket file
no longer exists and the entire IPC path runs through shared memory.

Consequences:

- The socket path can be reused immediately after the consumer calls
  `Bus.listen()` again — there is no TIME_WAIT or linger state.
- A second `connect()` to the same path after the handshake will get
  `ECONNREFUSED` — the socket is gone.
- Deleting the socket file manually has no effect on an established bus.
- On crash, the socket file may survive. Clean it up before restarting the
  listener: `rm -f /tmp/your.sock` or `os.unlink()`.

### Relisten pattern

To accept a new producer after the previous one disconnects, destroy the
existing bus and call `listen()` again on the same path. Each `listen()` call
creates a new SHM arena — ring buffer state does not persist across sessions.

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

`PeerDeadError` (Python) / `TachyonError::PeerDead` (Rust) /
`TACHYON_STATE_FATAL_ERROR` (C++) is raised when the consumer's watchdog
timeout fires. The timeout is **200 ms** — if the consumer spins or sleeps
waiting for a message and the producer has not written anything and its
heartbeat TSC has not advanced for 200 ms, the bus transitions to
`FatalError`.

The 200 ms threshold is intentionally conservative. It prevents false positives
on idle producers (e.g. a market data feed with no activity for several hundred
milliseconds). If you need faster dead-peer detection, call `flush()` explicitly
from the producer at a regular interval even when no data is being sent.

Conditions that trigger `PeerDeadError`:

- Producer process crashed without calling `tachyon_bus_destroy()`.
- Producer is live but blocked and has not written any message within the
  watchdog window.
- Producer is on a suspended VM or container.

Conditions that do **not** trigger `PeerDeadError`:

- Producer is slow but still writing — heartbeat advances on every
  `commit_tx()`.
- Consumer is the slow side — backpressure, not dead-peer.

### Restart pattern

The recommended supervisor loop keeps the listener alive indefinitely and
respawns the producer side from a process manager (systemd, supervisord,
custom watchdog).

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

The consumer owns the socket. It calls `listen()` in a loop. The producer
connects, sends, and exits (or crashes). The consumer detects the dead peer,
destroys the bus, and calls `listen()` again. The socket is recreated on each
`listen()` call — the producer does not need to do anything special.

### NUMA binding

If producer and consumer are on different NUMA nodes, all ring buffer accesses
cross the interconnect. Call `set_numa_node()` immediately after `listen()` or
`connect()` to migrate the SHM pages before the hot path begins.

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

The ring buffer must be large enough to absorb producer bursts during consumer
pauses (batch compute, GC, scheduling preemption).

### Formula

```
CAPACITY ≥ max_burst_messages × aligned_message_size
```

Where `aligned_message_size` is:

```
aligned = ceil((sizeof(MessageHeader) + payload_bytes) / 64) × 64
```

`sizeof(MessageHeader)` is 64 bytes (`TACHYON_MSG_ALIGNMENT`). Capacity must
be a **power of two**.

### Examples

| Use case                          | Payload      | Max burst   | Aligned slot | Minimum capacity   |
|-----------------------------------|--------------|-------------|--------------|--------------------|
| Market ticks                      | 32 bytes     | 10 000 msgs | 128 bytes    | 2 MB (`1 << 21`)   |
| ML feature vectors                | 1 024 bytes  | 512 frames  | 1 088 bytes  | 1 MB (`1 << 20`)   |
| Audio frames (1024 samples × f32) | 4 096 bytes  | 64 frames   | 4 160 bytes  | 512 KB (`1 << 19`) |
| Large blobs                       | 65 536 bytes | 8 msgs      | 65 600 bytes | 1 MB (`1 << 20`)   |

Round up to the next power of two and add a 2× safety margin for bursty
producers.

### Practical defaults

```python
# Market data — low latency, small payload
CAPACITY = 1 << 20  # 1 MB

# ML inference — larger payload, Python consumer pause during matmul
CAPACITY = 1 << 23  # 8 MB

# Audio / video pipeline — large frames, real-time consumer
CAPACITY = 1 << 22  # 4 MB
```

If the producer returns `TachyonError` / `TACHYON_ERR_FULL`, the buffer is
too small for the burst rate — increase capacity or reduce burst size. The
anti-overwrite shield never drops messages silently; the producer blocks or
returns an error instead.

### Memory cost

Tachyon uses `memfd_create` + `mmap(MAP_POPULATE)`, which allocates physical
pages at `listen()` time. `CAPACITY = 1 << 23` (8 MB) costs 8 MB of RAM in the
producer process and 8 MB in the consumer process (two `mmap` mappings of the
same `memfd`). The physical pages are shared — total RAM cost is 8 MB, not 16.