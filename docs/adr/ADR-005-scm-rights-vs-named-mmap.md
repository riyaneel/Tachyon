# ADR-005: SCM_RIGHTS vs named shared memory

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

The producer creates a shared memory region and must hand the consumer a way to map it. Two approaches exist:

- **Named shared memory** (`shm_open` with a well-known path, or a path negotiated out-of-band) leaves a `/dev/shm/name`
  entry on the filesystem. Any process with filesystem access to `/dev/shm` can open the name, regardless of whether the
  producer intended to share it with that process. The producer must `unlink` the name after the consumer connects, but
  there is a race between the consumer opening the name and the producer unlinking it. If the producer crashes before
  unlinking, the name persists indefinitely. Cleanup is the operator's responsibility.

- **`SCM_RIGHTS`** transfers a file descriptor over a UNIX domain socket via an ancillary `cmsg`. The fd arrives in the
  consumer process as a new, independent file descriptor pointing to the same underlying kernel object. No filesystem
  path is needed or created. The UDS socket itself serves as the rendezvous point, and Tachyon's UDS lifecycle is
  already one-shot: `listen → accept → sendmsg → close`. After `sendmsg`, the socket is discarded. Only the consumer
  holding the received fd can map the SHM region.

The `TachyonHandshake` struct (`magic`, `version`, `capacity`, `shm_size`, `msg_alignment`) is sent in the `iovec` of
the same `sendmsg` call that carries the fd, so the ABI check and the fd transfer happen atomically from the socket
layer's perspective.

## Decision

Use `SCM_RIGHTS` over a UNIX domain socket. The socket path is used only as a rendezvous address; it is `unlink`ed by
the listener before `bind` (clearing stale sockets from previous runs) and the socket file is removed immediately after
`accept` completes. The SHM region is identified solely by the transferred fd; no filesystem path for the SHM object
exists at any point after the handshake.

## Consequences

**Positive**

- No `/dev/shm` entry at any point. A crashed producer leaves no SHM file to clean up; the kernel releases the memory
  when the last fd referencing it is closed.
- Access to the SHM region is capability-based: only the process that received the fd via `SCM_RIGHTS` can map it. No
  ambient authority via `/dev/shm` path guessing.
- The handshake and fd transfer happen in a single `sendmsg`/`recvmsg` round trip. There is no second connection, no
  second socket, no second syscall sequence.
- The socket file is cleaned before `bind`, making the listener safe to restart immediately after a crash without manual
  `rm -f`.

**Negative**

- `SCM_RIGHTS` is POSIX but not universally implemented identically. On Linux, the received fd is always a new
  descriptor; on macOS it behaves the same way. On older Solaris or AIX it is not available. Tachyon targets Linux
  (primary) and macOS (tier-2) only.
- The UDS socket path is still visible on the filesystem for the duration of the `listen` call. If the process is killed
  between `bind` and `accept`, the socket file persists until the next `listen` on the same path (which calls `unlink`
  before `bind`). Operators must clean up stale socket files if they switch to a different path permanently.
- Cross-machine IPC is not supported. `SCM_RIGHTS` is a local IPC primitive. Distributed transports are explicitly
  deferred post-v1.0.

**Neutral**

- The consumer calls `close(received_fd)` after `mmap` to avoid leaking the fd. The mapping remains valid; `close` on a
  `memfd` does not unmap existing mappings.
- The socket path passed to `tachyon_bus_listen` is reused as the `memfd_create` name (Linux) for debuggability, it
  appears in `/proc/self/fd` as the memfd label.
