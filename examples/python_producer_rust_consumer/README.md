# python_producer_rust_consumer

Python process pushes market data ticks via `send_zero_copy`. Rust process
reads them using the raw C API. No serialization. No kernel copies after the
UDS handshake.

## Results

Hardware: i7-12650H · DDR5-5600 · Ubuntu 24.04 · no CPU isolation.

| Metric                         | Value                   |
|--------------------------------|-------------------------|
| Messages                       | 500 000                 |
| Payload                        | 32 bytes (`MarketTick`) |
| Producer throughput            | ~1 060 000 msg/sec      |
| One-way latency (steady state) | 14–16 µs                |
| One-way latency (spikes)       | 30–55 µs                |

Spikes are scheduler preemption on the Python side, not Tachyon overhead. Throughput drops to ~1.47M msg/sec without the
`time.time_ns()` timestamp.

## Wire format

```
MarketTick  32 bytes  little-endian
  [0..8]    symbol      [u8; 8]   e.g. b"BTCUSDT\0"
  [8..16]   price       f64
  [16..20]  quantity    u32
  [20..28]  timestamp   u64       nanoseconds, time.time_ns()
  [28..32]  padding     [u8; 4]
```

`type_id = 1` for BID, `type_id = 2` for ASK, `type_id = 0` for sentinel.

## Build

```bash
# Python producer — no build step
pipenv install tachyon-ipc

# Rust consumer
cd examples/python_producer_rust_consumer/consumer
cargo build --release
```

## Run

```bash
# terminal 1 — consumer first (owns the socket)
cd examples/python_producer_rust_consumer/consumer
cargo run --release

# terminal 2
pipenv run python examples/python_producer_rust_consumer/producer.py
```

## Send pattern

Zero-copy write: `send_zero_copy` acquires a `TxGuard` over the ring buffer slot, writes directly into shared memory via
`memoryview`, sets `actual_size`, and flushes every 64 messages.

```python
with bus.send_zero_copy(size=TICK_SIZE, type_id=tick_type) as tx:
    with memoryview(tx) as mv:
        struct.pack_into(FMT, mv, 0, symbol, price, qty, ts)
    tx.actual_size = TICK_SIZE

if (i + 1) % FLUSH_BATCH == 0:
    bus._bus.flush()
```

## Read pattern

Rust reads via the raw C API. The `MarketTick` struct is `repr(C, packed)` — fields are read with `read_unaligned` to
avoid UB on unaligned access.

```rust
let ptr = tachyon_acquire_rx_blocking(rx, & mut type_id, & mut size, 10_000);
let tick = unsafe {
    let base = ptr as * const u8;
    MarketTick {
        price: base.add(8).cast::< f64 > ().read_unaligned(),
        quantity: base.add(16).cast::< u32 > ().read_unaligned(),
        timestamp: base.add(20).cast::< u64 > ().read_unaligned(),
    }
};
tachyon_commit_rx(rx);
```
