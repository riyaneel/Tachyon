# cpp_producer_cpp_consumer

Inter-process RTT benchmark. Two processes, two SPSC ring buffers over `memfd` shared memory. The hot path contains no
syscalls, no allocations, and no kernel involvement after the UDS handshake.

## Results

Hardware: i7-12650H · DDR5-5600 · Ubuntu 24.04 · cores 8 and 9 pinned ·`SCHED_FIFO` priority 99 · `mlockall` · no
`isolcpus`.

```
┌─────────────────────────────────────────────────┐
│  Tachyon SHM — inter-process RTT benchmark      │
│  Payload:   32 bytes   Samples:   1000000       │
│  Cores:   ping= 8  pong= 9   rdtsc / spin-only  │
├──────────────────────────────────┬──────────────┤
│  Metric                          │     RTT (ns) │
├──────────────────────────────────┼──────────────┤
│  Min                              │        95.2 │
│  p50  (median)                    │       124.3 │
│  p90                              │       191.2 │
│  p99                              │       205.4 │
│  p99.9                            │       236.6 │
│  p99.99                           │       509.7 │
│  Max                              │      4938.3 │
├──────────────────────────────────┼──────────────┤
│  Mean                             │       141.2 │
│  Std dev                          │        39.8 │
├──────────────────────────────────┼──────────────┤
│  One-way p50 estimate             │        62.1 │
│  Throughput (K RTT/s)             │      6686.4 │
└──────────────────────────────────┴──────────────┘
```

One-way p50: **62 ns**. p99.99 at 509 ns is scheduler jitter — `isolcpus=8,9`
brings it below 200 ns.

## Build

Requires GCC ≥ 14 or Clang ≥ 17.

```bash
for dir in ping pong; do
    cmake -S examples/cpp_producer_cpp_consumer/$dir \
          -B examples/cpp_producer_cpp_consumer/$dir/build \
          -DCMAKE_BUILD_TYPE=Release
    cmake --build examples/cpp_producer_cpp_consumer/$dir/build -j$(nproc)
done
```

## Run

```bash
rm -f /tmp/tachyon_pa.sock /tmp/tachyon_ap.sock

# terminal 1
sudo ./pong/build/tachyon_pong

# terminal 2
sudo ./ping/build/tachyon_ping
```

Start order is arbitrary — both sides retry until connected. `sudo` is required
for `SCHED_FIFO` and `mlockall`; omitting it degrades tail latency.

## Handshake

`tachyon_bus_listen()` blocks on `accept()`. With two buses in opposing
directions a sequential handshake deadlocks. Each process runs `listen` in a
background thread while `main` retries `connect` at 50 ms intervals.

```
ping                                pong
────                                ────
thread → listen(SOCK_PA)  ←───  connect(SOCK_PA) retrying
connect(SOCK_AP) retrying  ───→  thread → listen(SOCK_AP)
```

## Measurement

Timing uses `__rdtsc()` calibrated against `CLOCK_MONOTONIC` over 10 ms.
`rdtsc` overhead is ~0.37 ns on this machine; `clock_gettime(CLOCK_MONOTONIC)`
costs ~25 ns — a 40% distortion at p50 RTT of 124 ns.

## Tuning

| Parameter                 | Value                       | Notes                                                    |
|---------------------------|-----------------------------|----------------------------------------------------------|
| `PING_CORE` / `PONG_CORE` | 8 / 9                       | Must be distinct physical cores. Verify with `lscpu -e`. |
| `SCHED_FIFO` priority     | 99                          | Prevents preemption mid-RTT.                             |
| `mlockall`                | `MCL_CURRENT \| MCL_FUTURE` | No page faults on hot path.                              |
| `isolcpus=8,9`            | kernel cmdline              | Eliminates OS jitter. Cuts p99.9 to ~150 ns.             |
| `CAPACITY`                | `1 << 16` (64 KB)           | Holds 512 in-flight 32-byte frames. Not the bottleneck.  |
| `WARMUP`                  | 10 000                      | Fills caches and branch predictors before measurement.   |
