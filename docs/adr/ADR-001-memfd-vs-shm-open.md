# ADR-001: `memfd_create` vs `shm_open`

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

Tachyon needs an anonymous shared memory region that can be transferred to another process via a file descriptor over a
UNIX domain socket (`SCM_RIGHTS`). Two POSIX APIs are available:

- `shm_open` creates a named object under `/dev/shm`. The name persists until explicitly unlinked, which introduces a
  race window between `shm_open` and `unlink`. On a crash, the stale file remains on the filesystem. Permissions are
  enforced via the filesystem, requiring `fchmod` to restrict access. The name is globally visible to any process with
  access to `/dev/shm`, expanding the attack surface.

- `memfd_create` (Linux 3.17+) creates a purely anonymous file backed by anonymous memory. No filesystem entry exists at
  any point. The file descriptor is the only handle; access requires explicit `SCM_RIGHTS` transfer.
  `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL` prevent resizing after creation, turning the SHM region into an
  immutable-size buffer once formatted. `MAP_POPULATE` on `mmap` pre-faults all pages at creation time, eliminating
  page-fault latency on the first hot-path access. `MADV_DONTFORK` prevents copy-on-write inheritance in Python
  `multiprocessing` child processes, which would silently map the ring buffer into worker processes that have no
  business touching it.

macOS does not provide `memfd_create`. The only anonymous-SHM equivalent is `shm_open` with an immediate `unlink` after
creation, which is functionally equivalent for the lifetime model Tachyon needs (fd-based, no persistent name) but lacks
seals and `MAP_POPULATE`.

## Decision

Use `memfd_create` on Linux. On macOS, use `shm_open` with an immediate `unlink` and a `/tmp`-scoped path
(`/tachyon-XXXXXX`). The sealing and `MAP_POPULATE` code paths are guarded with `#if defined(__linux__)`. The public
API (`SharedMemory::create`) is identical on both platforms; the implementation difference is confined to `shm.cpp`.

## Consequences

**Positive**

- No `/dev/shm` cleanup required on a crash; fd lifecycle is tied to process lifetime.
- Seals prevent accidental resize after format, catching bugs in `Arena::format` callers.
- `MAP_POPULATE` eliminates first-access page faults on Linux, keeping ring buffer latency consistent from the first
  message.
- `MADV_DONTFORK` makes Python `multiprocessing` safe without extra user-side precautions.

**Negative**

- Requires Linux 3.17+. Kernels older than that (RHEL 7 stock: 3.10) are not supported. In practice, `memfd_create` has
  been universally available since Ubuntu 16.04 / Debian 9.
- macOS does not benefit from seals or `MAP_POPULATE`; the first few messages on macOS may incur page-fault latency not
  present on Linux.

**Neutral**

- The fd passed via `SCM_RIGHTS` is valid regardless of whether the underlying object was created with `memfd_create` or
  `shm_open`; the consumer side (`SharedMemory::join`) is platform-agnostic.
- `fchmod(fd, 0600)` is called on Linux for defense-in-depth even though `memfd` objects are not accessible via the
  filesystem; it is a no-op in practice but documents intent.
