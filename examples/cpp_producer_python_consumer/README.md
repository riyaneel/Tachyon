# cpp_producer_python_consumer

C++ process pushes `f32[256]` feature vectors into a Tachyon ring buffer.
Python process drains them into a pre-allocated NumPy buffer and runs batched
inference. Identical wire format and consumer pattern to
[rust_producer_python_consumer](../rust_producer_python_consumer) — the
difference is the producer.

## Results

Hardware: i7-12650H · DDR5-5600 · Ubuntu 24.04 · no CPU isolation.

| Metric               | Value                    |
|----------------------|--------------------------|
| Frames               | 500 000                  |
| Payload per frame    | 1 024 bytes (`f32[256]`) |
| Batch size (compute) | 256 frames               |
| Producer throughput  | 374 858 frames/sec       |
| Consumer throughput  | 370 877 frames/sec       |
| Bandwidth            | 0.38 GB/s                |

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

# C++ producer — requires GCC ≥ 14 or Clang ≥ 17
cd examples/cpp_producer_python_consumer/producer
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
# terminal 1 — consumer first (owns the socket)
pipenv run python examples/cpp_producer_python_consumer/consumer.py

# terminal 2
./examples/cpp_producer_python_consumer/producer/build/tachyon_cpp_producer
```

## C++ vs Rust producer

The C++ producer is faster than the Rust producer (no FFI layer, no safety
checks at the call site), which means it saturates the ring buffer more
frequently. Saturation forces the producer into the 10 µs sleep branch of the
backpressure loop more often, intermittently starving the Python consumer of
CPU time. This is why throughput is lower (370K vs 466K f/s) despite the
producer being intrinsically faster.

The +73% gain over the naive implementation (214K → 370K f/s) came from a
single fix: removing `tachyon_flush()` from the backpressure loop.
`tachyon_flush()` acquires `consumer_lock` internally; calling it while the
buffer is full prevented `acquire_rx()` from draining the ring.

```cpp
// Correct backpressure — no lock contention
static void *acquire_with_backpressure(tachyon_bus_t *bus, size_t size) {
    for (unsigned spins = 0;;) {
        void *ptr = tachyon_acquire_tx(bus, size);
        if (ptr) return ptr;
        // Do NOT call tachyon_flush() here.
        if (++spins < 32) { __asm__ volatile("pause" ::: "memory"); }
        else { std::this_thread::sleep_for(std::chrono::microseconds(10)); spins = 0; }
    }
}
```

## Receive pattern

Same two-phase pattern as the Rust example. `consumer_lock` held for ~100 ns
per frame; inference runs outside the SHM context.

```python
with raw_bus.acquire_rx() as rx:  # consumer_lock held ~100 ns
    with memoryview(rx) as mv:
        accum[batch_idx] = np.frombuffer(mv, dtype=np.float32)

if batch_idx == BATCH_SIZE:  # inference outside SHM
    out = torch.from_numpy(accum) @ WEIGHTS
```
