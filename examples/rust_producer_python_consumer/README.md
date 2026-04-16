# rust_producer_python_consumer

Rust process pushes `f32[256]` feature vectors into a Tachyon ring buffer. Python process drains them into a
pre-allocated NumPy buffer and runs batched inference. No serialization. No kernel copies after the UDS handshake.

## Results

Hardware: i7-12650H · DDR5-5600 · Fedora 43 · Linux 6.19.11 · no CPU isolation.

| Metric               | Value                    |
|----------------------|--------------------------|
| Frames               | 500 000                  |
| Payload per frame    | 1 024 bytes (`f32[256]`) |
| Batch size (compute) | 256 frames               |
| Producer throughput  | 469 598 frames/sec       |
| Consumer throughput  | 466 035 frames/sec       |
| Bandwidth            | 0.48 GB/s                |

Consumer throughput by backend:

| Backend     | Throughput | Bandwidth   | Compute                             |
|-------------|------------|-------------|-------------------------------------|
| PyTorch     | ~466K f/s  | ~0.48 GB/s  | `torch.from_numpy(accum) @ WEIGHTS` |
| NumPy       | ~600K f/s  | ~0.62 GB/s  | `accum @ WEIGHTS`                   |
| Pure Python | ~15K f/s   | ~0.015 GB/s | `struct.unpack` per frame           |

## Wire format

```
FeatureVector  1024 bytes  little-endian
  [0..1024]    f32[256]    value = frame_idx + i * 0.001

Sentinel       1024 bytes  all zeros  type_id = 0
```

## Build

```bash
# Python consumer
pipenv install torch   # or: pipenv install numpy

# Rust producer
cd examples/rust_producer_python_consumer/producer
cargo build --release
```

## Run

```bash
# terminal 1 — consumer first (owns the socket)
pipenv run python examples/rust_producer_python_consumer/consumer.py

# terminal 2
./examples/rust_producer_python_consumer/producer/target/release/producer
```

Producer retries `connect` up to 100 times at 50 ms intervals.

## Receive pattern

`consumer_lock` is held only for the duration of one header read and one
1024-byte row copy into the accumulation buffer (~100 ns). Inference runs
outside the SHM context.

```python
# Phase 1 — SHM access (~100 ns, consumer_lock held)
with raw_bus.acquire_rx() as rx:
    with memoryview(rx) as mv:
        accum[batch_idx] = np.frombuffer(mv, dtype=np.float32)

# Phase 2 — inference (~200 µs, no lock)
if batch_idx == BATCH_SIZE:
    out = torch.from_numpy(accum) @ WEIGHTS
```

The 3.7× gap between naive per-frame (125K f/s) and batched (466K f/s) is
entirely due to amortizing the CPython boundary cost across 256 frames. The
boundary itself costs ~2 µs per `acquire_rx` call regardless of payload size.

## Backpressure

When the ring buffer is full the producer spins on `cpu_relax()` for 32
iterations then sleeps for 10 µs. It does not call `flush()` during
backpressure — `flush()` acquires `consumer_lock` internally, which would
starve `acquire_rx()` in the Python consumer and prevent draining.

```rust
fn acquire_with_backpressure(bus: &Bus, size: usize) -> TxGuard {
    let mut spins: u32 = 0;
    loop {
        if let Ok(g) = bus.acquire_tx(size) { return g; }
        spins += 1;
        if spins < 32 { std::hint::spin_loop(); } else {
            std::thread::sleep(Duration::from_micros(10));
            spins = 0;
        }
    }
}
```
