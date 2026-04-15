# Tachyon

[![CI](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml/badge.svg)](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/tachyon-ipc)](https://pypi.org/project/tachyon-ipc/)
[![Crates.io](https://img.shields.io/crates/v/tachyon-ipc)](https://crates.io/crates/tachyon-ipc)
[![Go Reference](https://pkg.go.dev/badge/github.com/riyaneel/tachyon/bindings/go.svg)](https://pkg.go.dev/github.com/riyaneel/tachyon/bindings/go)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](./LICENSE)

Tachyon is a bare-metal, lock-free IPC primitive. Strictly-bounded SPSC ring buffer over POSIX shared memory, with
zero-copy bindings for Python, Rust, and C++.

---

- [When to use](#when-to-use-tachyon)
- [Install](#install)
- [Quickstart](#quickstart)
- [Benchmarks](#benchmarks)
- [Examples](#examples)
- [Architecture](#architecture)
- [Requirements](#requirements)

---

## When to use Tachyon

- **ML inference pipeline**: a C++ or Rust process generates feature vectors faster than Python can consume them.
  Tachyon lets PyTorch read directly from
  shared memory via DLPack or `memoryview`, with no serialization and no kernel copies on the hot path.
- **Trading feed**: a native order book process pushes market data ticks at 1M+ msg/sec to a Python strategy. Zero-copy
  `send_zero_copy` + typed `type_id` routing keeps the producer below 100 ns per message.
- **Audio / video inter-process**: a real-time encoder or DSP process pushes fixed-size frames to a consumer on the
  same machine. The SPSC ring absorbs bursts during consumer pauses without dropping frames or blocking the producer.

---

## Install

**Python**: compiles the C++ core at install time, requires GCC 14+ or Clang 17+:

```bash
pip install tachyon-ipc
```

> **Note:** the PyPI package is `tachyon-ipc`, not `tachyon` (which is an unrelated quantum simulator). Always install
> with `pip install tachyon-ipc`.

**Rust:**

```bash
cargo add tachyon-ipc
```

**C++ (CMake FetchContent):**

```cmake
include(FetchContent)

FetchContent_Declare(tachyon
		GIT_REPOSITORY https://github.com/riyaneel/tachyon.git
		GIT_TAG v0.3.0
)
FetchContent_GetProperties(tachyon)
if (NOT tachyon_POPULATED)
	FetchContent_Populate(tachyon)
	add_subdirectory(${tachyon_SOURCE_DIR}/core ${tachyon_BINARY_DIR}/tachyon-core)
endif ()

target_link_libraries(my_app PRIVATE tachyon)
```

---

## Quickstart

### Python: Standard API

Two terminals, two processes.

```bash
# terminal 1 - consumer first (owns the socket)
python3 - <<'EOF'
import tachyon
with tachyon.Bus.listen("/tmp/demo.sock", 1 << 16) as bus:
    msg = next(iter(bus))
    print(f"received type_id={msg.type_id} data={msg.data}")
EOF

# terminal 2
python3 - <<'EOF'
import tachyon
with tachyon.Bus.connect("/tmp/demo.sock") as bus:
    bus.send(b"hello tachyon", type_id=1)
EOF
```

### Python: Zero-Copy

```bash
# terminal 1
python3 - <<'EOF'
import tachyon
with tachyon.Bus.listen("/tmp/demo_zc.sock", 1 << 16) as bus:
    with bus.recv_zero_copy() as rx:
        with memoryview(rx) as mv:
            print(f"received {mv.tobytes()}")
EOF

# terminal 2
python3 - <<'EOF'
import tachyon
payload = b"zero_copy_payload"
with tachyon.Bus.connect("/tmp/demo_zc.sock") as bus:
    with bus.send_zero_copy(size=len(payload), type_id=42) as tx:
        with memoryview(tx) as mv:
            mv[:] = payload
        tx.actual_size = len(payload)
EOF
```

### Python: DLPack / PyTorch

```bash
# terminal 1
python3 - <<'EOF'
import torch, tachyon
with tachyon.Bus.listen("/tmp/demo_dl.sock", 1 << 16) as bus:
    with bus.drain_batch() as batch:
        tensor = torch.from_dlpack(batch[0]).view(torch.float32)
        print(tensor)  # tensor([1., 2., 3., 4.])
        del tensor
EOF

# terminal 2
python3 - <<'EOF'
import struct, tachyon
data = struct.pack("4f", 1.0, 2.0, 3.0, 4.0)
with tachyon.Bus.connect("/tmp/demo_dl.sock") as bus:
    with bus.send_zero_copy(size=len(data), type_id=1) as tx:
        with memoryview(tx) as mv:
            mv[:] = data
        tx.actual_size = len(data)
EOF
```

### Rust

```rust
use std::thread;
use tachyon_ipc::Bus;

const SOCK: &str = "/tmp/demo_rust.sock";
const CAP: usize = 1 << 16;

fn main() {
    let srv = thread::spawn(|| {
        let bus = Bus::listen(SOCK, CAP).unwrap();
        let guard = bus.acquire_rx(10_000).unwrap();
        println!("received {} bytes, type_id={}", guard.actual_size, guard.type_id);
        guard.commit().unwrap();
    });

    thread::sleep(std::time::Duration::from_millis(20));

    let bus = Bus::connect(SOCK).unwrap();
    bus.send(b"hello tachyon", 1).unwrap();

    srv.join().unwrap();
}
```

### C++

```c++
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>
#include <cstring>

using namespace tachyon::core;

int main() {
    constexpr size_t CAPACITY = 4096;
    constexpr size_t SHM_SIZE = sizeof(MemoryLayout) + CAPACITY;

    auto shm      = SharedMemory::create("demo", SHM_SIZE).value();
    auto producer = Arena::format(shm.data(), CAPACITY).value();
    auto consumer = Arena::attach(shm.data()).value();

    std::byte *tx = producer.acquire_tx(32);
    std::memset(tx, 0xAB, 32);
    producer.commit_tx(32, /*type_id=*/1);
    producer.flush();

    uint32_t type_id = 0;
    size_t   actual  = 0;
    const std::byte *rx = consumer.acquire_rx(type_id, actual);
    consumer.commit_rx();
}
```

---

## Benchmarks

Ping-pong RTT, two processes, 32-byte payload, 1 000 000 samples.  
**Machine:** Intel Core i7-12650H, 64 GiB DDR5-5600 SODIMM.  
**Build:** GCC 14, Release, `SCHED_FIFO` priority 99, `mlockall`, cores 8/9 pinned.

| Percentile | Latency  |
|------------|----------|
| Min        | 51.3 ns  |
| p50        | 56.5 ns  |
| p90        | 101.2 ns |
| p99        | 112.4 ns |
| p99.9      | 122 ns   |
| p99.99     | 467.3 ns |
| Max        | 4 938 ns |

**Throughput: 13 229 K RTT/sec · One-way p50: 28.3 ns**

p99.99 reflects scheduler jitter on an untuned kernel. With `isolcpus=8,9`, the tail converges toward the p99 band.

---

## Examples

End-to-end cross-language examples in [`examples/`](./examples). Each runs in
two terminals and uses a typed payload with a sentinel shutdown signal.

| Example                                                                   | Producer | Consumer       | Throughput                       | Payload                |
|---------------------------------------------------------------------------|----------|----------------|----------------------------------|------------------------|
| [cpp_producer_cpp_consumer](./examples/cpp_producer_cpp_consumer)         | C++      | C++            | **13 229 K RTT/s** · p50 56.5 ns | 32 bytes               |
| [python_producer_rust_consumer](./examples/python_producer_rust_consumer) | Python   | Rust           | **1 060 K msg/s**                | 32 bytes `MarketTick`  |
| [rust_producer_python_consumer](./examples/rust_producer_python_consumer) | Rust     | Python (torch) | **510 K frames/s** · 0.51 GB/s   | 1 024 bytes `f32[256]` |
| [cpp_producer_python_consumer](./examples/cpp_producer_python_consumer)   | C++      | Python (torch) | **533 K frames/s** · 0.53 GB/s   | 1 024 bytes `f32[256]` |

All numbers: i7-12650H · DDR5-5600 · Ubuntu 24.04 · no CPU isolation (except `cpp_producer_cpp_consumer` which uses
`SCHED_FIFO` + core pinning).

---

## Architecture

Tachyon decouples the **control plane** (connection bootstrap) from the **data plane** (hot-path I/O).

**Control plane.** Process discovery and the initial ABI handshake run over a Unix domain socket. The socket transfers
an anonymous `memfd` file descriptor via `SCM_RIGHTS`, then is permanently discarded. If the producer and consumer were
compiled with differing `TACHYON_MSG_ALIGNMENT` values, the connection is rejected before the first byte of data is
exchanged.

**Data plane.** All subsequent I/O operates directly in the shared memory segment with no kernel involvement. The SPSC
ring uses `memory_order_acquire` / `memory_order_release` atomics with amortized batch publication: the shared head/tail
indices are updated at most once every 32 messages or on an explicit `flush()`.

**Hardware sympathy.** Every control structure (message headers, atomic indices, watchdog flags) is padded to 64-byte
or 128-byte boundaries. False sharing between producer and consumer cache lines is structurally impossible.

**Hybrid wait strategy.** The consumer spins for a bounded threshold (`cpu_relax()`), then sleeps via `SYS_futex` (
Linux) or `__ulock_wait` (macOS) with a 200 ms watchdog timeout. Kernel sleeps are bounded, so the thread periodically
returns to the host runtime to process signals.

**Zero-copy contract.** C++ and Rust expose raw pointers or slices tied to the ring buffer lifetime. Python surfaces the
buffer protocol (`memoryview`) and DLPack (`__dlpack__`), allowing PyTorch, JAX, and NumPy to consume payloads directly
from shared memory without copying.

For wire protocol details and ABI guarantees → [`ABI.md`](./ABI.md).  
For socket lifecycle, supervision patterns, and capacity sizing → [`INTEGRATION.md`](./INTEGRATION.md).

---

## Requirements

| Component | Minimum                                                    |
|-----------|------------------------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2)                  |
| Compiler  | Clang 17+ for basic use, Clang 21+ for preset-based builds |
| CMake     | 3.31+                                                      |
| Python    | 3.10+                                                      |
| Rust      | stable (2024 edition)                                      |

---

## License

[Apache 2.0](./LICENSE)
