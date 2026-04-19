# Syscall Reference

Tachyon IPC lifecycle splits into three phases with very different syscall profiles. All observations are derived from
`strace` and the core source (`core/`). macOS differences are noted explicitly.

## Handshake (listen)

Called once per `tachyon_bus_listen()`. All syscalls are one-shot except poll.

| Syscall                  | Source                              | Reason                                                         | Conditional |
|--------------------------|-------------------------------------|----------------------------------------------------------------|-------------|
| `memfd_create`           | `shm.cpp::SharedMemory::create`     | Creates the anonymous SHM region (visible in `/proc/self/fd`). | Linux only  |
| `ftruncate`              | `shm.cpp::SharedMemory::create`     | Allocates the ring buffer capacity.                            | Linux only  |
| `fchmod(0600)`           | `shm.cpp::SharedMemory::create`     | Restricts memfd permissions (no-op in practice).               | Linux only  |
| `fcntl(F_ADD_SEALS)`     | `shm.cpp::SharedMemory::create`     | Prevents resize after format (`F_SEAL_SHRINK\|GROW\|SEAL`).    | Linux only  |
| `mmap(MAP_SHARED)`       | `shm.cpp::SharedMemory::create`     | Maps the ring buffer.                                          |             |
| `mmap(MAP_POPULATE)`     | `shm.cpp::SharedMemory::create`     | Pre-faults all pages before handshake completes.               | Linux only  |
| `madvise(MADV_DONTFORK)` | `shm.cpp::SharedMemory::create`     | Prevents CoW inheritance in child processes.                   | Linux only  |
| `socket(AF_UNIX)`        | `transport_uds.cpp::uds_export_shm` | Opens the UDS endpoint.                                        |             |
| `unlink`                 | `transport_uds.cpp::uds_export_shm` | Clears stale socket before `bind`. `ENOENT` on first run.      |             |
| `bind`                   | `transport_uds.cpp::uds_export_shm` | Binds to the socket path.                                      |             |
| `listen`                 | `transport_uds.cpp::uds_export_shm` | Marks the socket as passive. Backlog of 1.                     |             |
| `poll`                   | `transport_uds.cpp::uds_export_shm` | 100 ms timeout loop until `accept` succeeds.                   |             |
| `accept`                 | `transport_uds.cpp::uds_export_shm` | One-shot. Listening socket closed immediately after.           |             |
| `sendmsg`                | `transport_uds.cpp::uds_export_shm` | Transfers handshake struct and memfd fd via `SCM_RIGHTS`.      |             |
| `close` x2               | `transport_uds.cpp::uds_export_shm` | Closes client socket and listening socket.                     |             |
| `unlink`                 | `transport_uds.cpp::uds_export_shm` | Removes the socket file. Path no longer exists after this.     |             |

On macOS, `memfd_create` / `ftruncate` / `fchmod` / `fcntl(F_ADD_SEALS)` are replaced by `shm_open` + immediate
`unlink` + `ftruncate`. The rest is identical.

## Handshake (connect)

Called once per `tachyon_bus_connect()`. Returns `TACHYON_ERR_NETWORK` immediately if the listener is not ready. Retry
is the caller's responsibility.

| Syscall                          | Source                              | Reason                                                                           | Conditional               |
|----------------------------------|-------------------------------------|----------------------------------------------------------------------------------|---------------------------|
| `socket(AF_UNIX)`                | `transport_uds.cpp::uds_import_shm` | Opens the UDS endpoint.                                                          |                           |
| `connect`                        | `transport_uds.cpp::uds_import_shm` | Fails with `ENOENT` if the listener is not ready.                                |                           |
| `close`                          | `transport_uds.cpp::uds_import_shm` | Closes the socket on connect failure.                                            | Failure path only         |
| `recvmsg`                        | `transport_uds.cpp::uds_import_shm` | Receives handshake struct and memfd fd via `SCM_RIGHTS`.                         |                           |
| `close`                          | `transport_uds.cpp::uds_import_shm` | Closes the UDS socket after `recvmsg`.                                           |                           |
| `mmap(MAP_SHARED\|MAP_POPULATE)` | `shm.cpp::SharedMemory::join`       | Maps the received memfd into the consumer address space and pre-faults all pages | `MAP_POPULATE` Linux only |
| `madvise(MADV_DONTFORK)`         | `shm.cpp::SharedMemory::join`       | Same as listen side.                                                             | Linux only                |

## Hot path (Linux)

- **Pure-spin mode** (`tachyon_bus_set_polling_mode(bus, 1)`): zero syscalls. Confirmed by strace.
- **Hybrid mode** (default): consumer spins up to `spin_threshold` iterations then sleeps via futex.

| Syscall             | Source                     | Reason                                                                 | Conditional                              |
|---------------------|----------------------------|------------------------------------------------------------------------|------------------------------------------|
| `futex(FUTEX_WAIT)` | `arena.cpp::platform_wait` | Consumer parks when the ring is empty. 200 ms timeout, then retry.     | Consumer only, under starvation          |
| `futex(FUTEX_WAKE)` | `arena.cpp::platform_wake` | Producer wakes the consumer on `flush_tx` if `consumer_sleeping == 1`. | Producer only, skipped in pure-spin mode |

## Hot path (macOS)

Same contract. `futex` is replaced by `__ulock_wait` / `__ulock_wake` (Apple primitives via `syscall()`). No seccomp BPF
on macOS.

## Configuration (post-handshake, one-shot)

| Syscall | Source                                     | Reason                                         | Conditional |
|---------|--------------------------------------------|------------------------------------------------|-------------|
| `mbind` | `tachyon_c.cpp::tachyon_bus_set_numa_node` | Migrates SHM pages to the requested NUMA node. | Linux only  |

## Teardown

Called once per `tachyon_bus_destroy()` when `ref_count` reaches zero.

| Syscall  | Source                           | Reason                            |
|----------|----------------------------------|-----------------------------------|
| `munmap` | `shm.cpp::SharedMemory::release` | Unmaps the SHM region.            |
| `close`  | `shm.cpp::SharedMemory::release` | Closes the memfd file descriptor. |

The kernel releases the anonymous memory when the last fd referencing it is closed. No filesystem cleanup is required.

## Binding notes

The syscall profile above applies to **C++/C and Rust only.** Every other binding runs with a managed runtime that emits
its own syscalls independently of Tachyon.

- **Go**: goroutine scheduler, GC, and netpoller emit `futex`, `clone`, `mmap`, `epoll_*` continuously. Any strict
  filter breaks the runtime.
- **Python**: GIL, GC, and signal handling emit arbitrary syscalls. The CPython extension is clean, the interpreter is
  not.
- **Java**: JVM emits `mmap`, `futex`, `clone`, `rt_sigaction` for thread and GC management. Panama FFM adds nothing,
  but the JVM baseline is wide.
- **Node.js**: libuv uses `epoll_wait`, `timerfd_*`, `eventfd`, and `io_uring` depending on version.

For polyglot deployments, apply containment at the process boundary via a supervisor (`systemd SystemCallFilter=`,
container seccomp profile) rather than from within the process. Pre-built profiles for C++/C only deployments are in
`contrib/seccomp/`.
