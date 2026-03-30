# Benchmark

Three binaries, one orchestrator, one comparator.

| Binary                | Scope           | Engine                                        |
|-----------------------|-----------------|-----------------------------------------------|
| `tachyon_bench_intra` | Intra-process   | Google Benchmark fixture, 1M iter × 3 reps    |
| `tachyon_bench_inter` | Inter-process   | Custom HDR loop, 1M iter, `fork()` + 2 arenas |
| `tachyon_bench_zmq`   | ZeroMQ baseline | Google Benchmark, `inproc://`, optional       |

Payload is 32 bytes for all three. This isolates transport overhead from serialization cost and keeps the message header
dominant in the slot budget.

---

## Build

```bash
# Standard release
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --parallel

# ZeroMQ baseline — install libzmq, CMake detects it via PkgConfig
sudo apt-get install libzmq3-dev
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release && cmake --build cmake-build-release --parallel

# PGO — two-phase, Clang or GCC
bash ci/bench/pgo.sh [build_dir] [parallelism]
```

---

## Run

```bash
# Full suite: intra + inter + ZeroMQ
bash ci/bench/run.sh [build_dir] [output_dir]

# Explicit core pinning
PING_CORE=8 PONG_CORE=9 bash ci/bench/run.sh
```

`ci/bench/run.sh` sets the CPU frequency governor to `performance` on the requested cores and restores it on exit via
`trap`. It pins both processes with`taskset -c` and applies `chrt -f 99` (SCHED_FIFO) if available — `sudo` or
`CAP_SYS_NICE` required. Results are written as timestamped JSON files under`benchmark/results/` and a p50 summary table
is printed on stdout.

| Variable     | Default   | Description                          |
|--------------|-----------|--------------------------------------|
| `PING_CORE`  | `8`       | Core for the client/benchmark thread |
| `PONG_CORE`  | `9`       | Core for the server/reflector thread |
| `ITERATIONS` | `1000000` | RTT sample count (inter only)        |
| `SCHED_PRIO` | `99`      | SCHED_FIFO priority                  |

---

## Compare

```bash
python3 ci/bench/compare.py baseline.json target.json [target2.json ...]

# Intra vs inter vs ZMQ
python3 ci/bench/compare.py \
    benchmark/results/intra_TIMESTAMP.json \
    benchmark/results/inter_TIMESTAMP.json \
    benchmark/results/zmq_TIMESTAMP.json
```

The comparator reads `p50_ns` … `p99.99_ns` counters from both `bench_intra` (Google Benchmark `state.counters`) and
`bench_inter` (hand-written JSON matching the same schema). Both formats are supported without configuration.

---

## Results

**Machine:** Intel Core i7-12650H · 64 GiB DDR5-5600 · Ubuntu 24.04 · GCC 14 · Release.  
**Setup:** `PING_CORE=8 PONG_CORE=9`, SCHED_FIFO, CPU governor `performance`. No `isolcpus`.

### Intra vs inter vs ZeroMQ

| Percentile | Intra (ns) | Inter (ns) | ZeroMQ inproc (ns) | Intra vs ZMQ |
|------------|:----------:|:----------:|:------------------:|:------------:|
| p50        |    107     |    134     |       6 751        |   **63×**    |
| p90        |    160     |    167     |       10 121       |     63×      |
| p99        |    178     |    201     |       10 553       |     59×      |
| p99.9      |    216     |    254     |       13 097       |     61×      |
| p99.99     |    926     |    953     |       17 390       |     19×      |

The intra/inter gap at p50 is +27 ns (+25%). This is the cost of the process boundary — the second SHM arena is mapped
via `SCM_RIGHTS`, and the hot path after handshake is identical in both cases: the same lock-free ring with no kernel
involvement.

ZeroMQ `inproc://` is a shared-memory transport within the same process — its strongest possible configuration. The 63×
gap at p50 reflects ZMQ's message framing, reference counting, and HWM bookkeeping. At p99.99 the gap narrows to 19×
because Tachyon's scheduler jitter floor (~1 µs) dominates both tails on an untuned kernel.

### PGO build

**Setup:** `bash ci/bench/pgo.sh`, Clang 18, `taskset -c 7,8,9`, SCHED_FIFO.

| Percentile | Intra (ns) |   Throughput    |
|------------|:----------:|:---------------:|
| p50        |     88     | 8 868 K RTT/sec |
| p99        |    138     |                 |
| p99.99     |    350     |                 |

PGO reduces p50 by 18% (107 → 88 ns). The gain comes from branch misprediction elimination in the ring buffer hot path —
specifically the`acquire_tx` capacity check and the `commit_tx` batch-flush threshold branch. Clang's
`fprofile-instr-generate` path typically yields slightly tighter tail latency than GCC due to more aggressive inlining
under feedback.

---

## Methodology

### Intra-process

Google Benchmark fixture with `UseManualTime()`. Each iteration measures`std::chrono::high_resolution_clock` around a
full ping-pong cycle:

- `acquire_tx` → `commit_tx` → `flush_tx` on the client thread,
- `acquire_rx_spin` → `commit_rx` → `acquire_tx` → `commit_tx` → `flush_tx` on the server thread pinned to
  `TACHYON_SERVER_CORE`. Three repetitions, median reported. 10 000 warmup iterations run in `SetUp()` before
  measurement begins.

Raw per-iteration latencies are sorted and attached as `state.counters["p50_ns"]` … `state.counters["p99.99_ns"]`.

### Inter-process

Custom HDR loop — `high_resolution_clock` around the full RTT, 1M samples collected then sorted in-process. Output is
hand-written JSON matching the Google Benchmark aggregate schema, so `compare.py` ingests both formats without
branching. The two processes communicate over two independent SHM arenas established via `fork()` + two sequential UDS
handshakes.

### ZeroMQ baseline

Same Google Benchmark fixture as intra, `ZMQ_PAIR` over `inproc://`, HWM set to 0 (unbounded). The server thread uses
`ZMQ_DONTWAIT` + `cpu_relax()` to match Tachyon's `acquire_rx_spin` spin behavior as closely as possible.

### Timing

`std::chrono::high_resolution_clock` is used for all benchmark binaries.
`examples/cpp_producer_cpp_consumer/` uses `__rdtsc()` calibrated against`CLOCK_MONOTONIC` over 10 ms for sub-nanosecond
resolution — the benchmark binaries use `high_resolution_clock` for portability across x86_64 and ARM64.

### Isolation

Without kernel `isolcpus`, p99.99 reflects scheduler preemption jitter (~1–5 µs). The recommended setup for reproducible
tail measurements:

```bash
# Kernel command line — requires reboot
isolcpus=8,9 nohz_full=8,9 rcu_nocbs=8,9

# Per-run
sudo cpupower -c 8,9 frequency-set -g performance
sudo bash ci/bench/run.sh
```

With `isolcpus=8,9`, p99.99 converges toward the p99 band (~200 ns inter, ~180 ns intra).

---

## JSON schema

Both binaries emit Google Benchmark-compatible JSON. Percentile counters are embedded as standard `state.counters`
fields and parsed by `compare.py`.

```json
{
  "benchmarks": [
    {
      "name": "IntraRTT/PingPong/median",
      "run_type": "aggregate",
      "aggregate_name": "median",
      "p50_ns": 107.0,
      "p90_ns": 160.0,
      "p99_ns": 178.0,
      "p99.9_ns": 216.0,
      "p99.99_ns": 926.0,
      "throughput_krtt_s": 9345.2
    }
  ]
}
```

Results accumulate under `benchmark/results/` (gitignored). `ci/bench/run.sh` creates the directory on first run.
