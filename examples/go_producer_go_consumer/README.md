# go_producer_go_consumer

Go process pushes 48-byte telemetry events into a Tachyon ring buffer. Another Go process drains them in large batches
and aggregates metrics natively. No serialization. No kernel copies after the UDS handshake.

## Results

Hardware: i7-12650H · DDR5-5600 · Fedora 43 · Linux 6.19.11 · no CPU isolation.

| Metric                 | Value                   |
|------------------------|-------------------------|
| Messages               | ~11 800 000             |
| Payload per message    | 48 bytes (`MetricSpan`) |
| Batch size (drain)     | 2048 messages           |
| Producer throughput    | ~350 000 msg/sec        |
| Steady state latency   | ~250 µs                 |
| Amortized CGO overhead | < 0.05 ns per message   |

Throughput is deliberately throttled by the producer's simulated workload delay. The CGO boundary costs ~60 ns per call;
fetching batches of 2048 messages virtually eliminates FFI overhead on the consumer side.

## Wire format

```
MetricSpan  48 bytes  little-endian
[0..8]    timestamp   u64       nanoseconds, time.Now().UnixNano()
[8..16]   trace_id    u64
[16..24]  span_id     u64
[24..28]  latency_ns  u32
[28..32]  cpu_usage   f32
[32..36]  mem_usage   f32
[36..37]  event_type  u8
[37..48]  padding     [u8; 7]
```

`type_id = 1` for MetricSpan.

## Build

Requires Go 1.23+ and a C++23 compliant compiler (GCC ≥ 14 or Clang ≥ 17).

```bash
cd examples/go_telemetry_agent

go build -o build/producer ./producer
go build -o build/consumer ./consumer
```

## Run

```bash
rm -f /tmp/tachyon_telemetry.sock

# terminal 1 — producer first (owns the socket)
./build/producer

# terminal 2
./build/consumer
```

Producer uses `Connect()` and will fail if the consumer is not already listening.

## Send pattern

Zero-copy write: `AcquireTx` returns a `TxGuard` over the ring buffer slot. The shared memory slice is directly cast to
the C-aligned Go struct via `unsafe.Pointer`.

```go
guard, err := bus.AcquireTx(structSize)
if err != nil {
    continue // Backpressure: drop the metric rather than blocking
}

span := (*common.MetricSpan)(unsafe.Pointer(&guard.Bytes()[0]))
span.Timestamp = uint64(time.Now().UnixNano())
// ... write fields ...

guard.Commit(structSize, common.TypeIDMetricSpan)
```

## Receive pattern

CGO Amortization: `DrainBatch` fetches up to 2048 messages in a single CGO call. The consumer iterates over the batch
using Go 1.23 `range-over-func`, casting raw SHM bytes to the struct natively without crossing the FFI boundary.

The hot path is pinned to an OS thread via `runtime.LockOSThread()` to prevent the Go scheduler from preempting the
goroutine while spinning on the futex, preserving L1/L2 cache locality.

```go
runtime.LockOSThread()

for {
    batch, _ := bus.DrainBatch(2048, 1000)
    
    for msg := range batch.Iter() {
        if msg.TypeID() == common.TypeIDMetricSpan {
            span := (*common.MetricSpan)(unsafe.Pointer(&msg.Data()[0]))
            // ... accumulate metrics locally ...
        }
    }

    batch.Commit() // Releases consumer_lock
}
```
