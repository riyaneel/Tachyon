# tachyon-ipc

Rust bindings for [Tachyon](https://github.com/riyaneel/Tachyon), a bare-metal, lock-free IPC primitive. SPSC ring
buffer over POSIX shared memory with sub-100ns p50 RTT.

## Install

    [dependencies]
    tachyon-ipc = "0.3.5"

Requires GCC 14+ or Clang 17+ at build time (the C++ core is compiled via `cc`).

## Quickstart

    use std::thread;
    use tachyon_ipc::Bus;

    const SOCK: &str = "/tmp/demo.sock";
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

## Batch drain

    let batch = bus.drain_batch(1024, 10_000).unwrap();
    for msg in &batch {
        println!("type_id={} size={}", msg.type_id(), msg.actual_size());
        // msg.data() is a zero-copy slice - lifetime tied to batch
    }
    batch.commit().unwrap();

## API surface

| Type               | Description                                             |
|--------------------|---------------------------------------------------------|
| `Bus`              | SPSC IPC bus. `Send` but not `Sync`, one per thread.    |
| `TxGuard<'_>`      | TX slot. Write then `commit()` or `commit_unflushed()`. |
| `RxGuard<'_>`      | RX slot. Read via `data()` then `commit()`.             |
| `RxBatchGuard<'_>` | Batch of RX slots. Iterate then `commit()`.             |
| `RxMsgView<'_>`    | Zero-copy view into one message inside a batch.         |
| `TachyonError`     | Error enum covering all failure modes.                  |

`Drop` on any uncommitted guard automatically rolls back or commits the transaction,
preventing lock starvation.

## NUMA binding

    // Bind SHM pages to NUMA node 0 immediately after connecting.
    // No-op on non-Linux platforms.
    bus.set_numa_node(0)?;

## Requirements

|          | Minimum                                   |
|----------|-------------------------------------------|
| OS       | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Compiler | GCC 14+ or Clang 17+                      |
| Rust     | stable (2024 edition)                     |

## License

Apache 2.0
