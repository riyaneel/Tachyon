# Tachyon

[![CI](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml/badge.svg)](https://github.com/riyaneel/tachyon/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/tachyon-ipc)](https://pypi.org/project/tachyon-ipc/)
[![Crates.io](https://img.shields.io/crates/v/tachyon-ipc)](https://crates.io/crates/tachyon-ipc)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](./LICENSE)

Tachyon is a bare-metal, lock-free IPC primitive. Strictly-bounded SPSC ring
buffer over POSIX shared memory, with zero-copy bindings for Python, Rust,
and C++.

---

## Install

**Python** — compiles the C++ core at install time, requires GCC 14+ or Clang 17+:

```bash
pip install tachyon-ipc
```

> **Note:** the PyPI package is `tachyon-ipc` — not `tachyon` (which is an unrelated quantum simulator). Always install
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
		GIT_TAG v0.1.0
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

### Python — Standard API

```python
import threading
import tachyon


def server():
    with tachyon.Bus.listen("/tmp/demo.sock", 1 << 16) as bus:
        msg = next(iter(bus))
        print(f"received type_id={msg.type_id} data={msg.data}")


t = threading.Thread(target=server)
t.start()

with tachyon.Bus.connect("/tmp/demo.sock") as bus:
    bus.send(b"hello tachyon", type_id=1)

t.join()
```

### Python — Zero-Copy

```python
import threading
import tachyon

payload = b"zero_copy_payload"


def server():
    with tachyon.Bus.listen("/tmp/demo_zc.sock", 1 << 16) as bus:
        with bus.recv_zero_copy() as rx:
            with memoryview(rx) as mv:
                data = mv.tobytes()  # single copy into Python heap


t = threading.Thread(target=server)
t.start()

with tachyon.Bus.connect("/tmp/demo_zc.sock") as bus:
    with bus.send_zero_copy(size=len(payload), type_id=42) as tx:
        with memoryview(tx) as mv:
            mv[:] = payload
        tx.actual_size = len(payload)

t.join()
```

### Python — DLPack / PyTorch

```python
import struct, threading
import torch, tachyon

data = struct.pack("4f", 1.0, 2.0, 3.0, 4.0)  # 16 bytes, 4× float32


def server():
    with tachyon.Bus.listen("/tmp/demo_dl.sock", 1 << 16) as bus:
        with bus.drain_batch() as batch:
            tensor = torch.from_dlpack(batch[0]).view(torch.float32)
            print(tensor)  # tensor([1., 2., 3., 4.])
            del tensor  # release before batch commits


t = threading.Thread(target=server)
t.start()

with tachyon.Bus.connect("/tmp/demo_dl.sock") as bus:
    with bus.send_zero_copy(size=len(data), type_id=1) as tx:
        with memoryview(tx) as mv:
            mv[:] = data
        tx.actual_size = len(data)

t.join()
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

```cpp
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

    // TX
    std::byte *tx = producer.acquire_tx(32);
    std::memset(tx, 0xAB, 32);
    producer.commit_tx(32, /*type_id=*/1);
    producer.flush();

    // RX
    uint32_t type_id = 0;
    size_t   actual  = 0;
    const std::byte *rx = consumer.acquire_rx(type_id, actual);
    consumer.commit_rx();
}
```

---

## Benchmarks

Ping-pong RTT, two threads, 32-byte payload, 1 000 000 samples.  
**Machine:** Intel Core i7-12650H, 64 GiB DDR5-5600 SODIMM.  
**Build:** GCC 14, PGO Release (`scripts/pgo_build.sh`), `taskset -c 7,8,9`.

| Percentile | Latency   |
|------------|-----------|
| Min        | 78 ns     |
| p50        | 93 ns     |
| p90        | 145 ns    |
| p99        | 155 ns    |
| p99.9      | 166 ns    |
| p99.99     | 350 ns    |
| Max        | 17 540 ns |

**Throughput: 8 553 K RTT/sec**

Max spikes reflect OS scheduler preemption on a non-isolated laptop core — not
a ring buffer pathology. On a server with isolated cores, p99.99 converges
toward the p99.9 band.

---

## Architecture

Tachyon decouples the **control plane** (connection bootstrap) from the
**data plane** (hot-path I/O).

**Control plane.** Process discovery and the initial ABI handshake run over
a Unix domain socket. The socket transfers an anonymous `memfd` file
descriptor via `SCM_RIGHTS`, then is permanently discarded. If the producer
and consumer were compiled with differing `TACHYON_MSG_ALIGNMENT` values,
the connection is rejected before the first byte of data is exchanged.

**Data plane.** All subsequent I/O operates directly in the shared memory
segment with no kernel involvement. The SPSC ring uses
`memory_order_acquire` / `memory_order_release` atomics with amortized
batch publication: the shared head/tail indices are updated at most once
every 32 messages or on an explicit `flush()`.

**Hardware sympathy.** Every control structure — message headers, atomic
indices, watchdog flags — is padded to 64-byte or 128-byte boundaries.
False sharing between producer and consumer cache lines is structurally
impossible.

**Hybrid wait strategy.** The consumer spins for a bounded threshold
(`cpu_relax()`), then sleeps via `SYS_futex` (Linux) or `__ulock_wait`
(macOS) with a 200 ms watchdog timeout. Kernel sleeps are bounded so the
thread periodically returns to the host runtime to process signals.

**Zero-copy contract.** C++ and Rust expose raw pointers or slices tied to
the ring buffer lifetime. Python surfaces the buffer protocol
(`memoryview`) and DLPack (`__dlpack__`), allowing PyTorch, JAX, and NumPy
to consume payloads directly from shared memory without copying.

For wire protocol details and ABI guarantees → [`ABI.md`](./ABI.md).

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Compiler  | GCC 14+ or Clang 17+                      |
| CMake     | 3.31+                                     |
| Python    | 3.10+                                     |
| Rust      | stable (2024 edition)                     |

---

## License

[Apache 2.0](./LICENSE)
