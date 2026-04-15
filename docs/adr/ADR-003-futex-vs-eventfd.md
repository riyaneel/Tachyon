# ADR-003: Futex vs eventfd for consumer sleep

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

When the ring buffer is empty, the consumer must not busy-spin indefinitely. After a configurable spin threshold, it
must sleep and be woken by the producer on the next `flush_tx`. Two kernel primitives are available for this on Linux:

- **`eventfd`** creates a file descriptor wrapping a 64-bit counter. The consumer calls `read(fd, ...)` to block; the
  producer calls `write(fd, 1)` to wake it. This requires two syscalls per wake cycle (one `write` from the producer,
  one `read` from the consumer), and the `eventfd` fd must be created at bus initialization time and passed to the
  consumer through some channel (embedded in the SHM region or via a second `SCM_RIGHTS` transfer). `poll`/`epoll`
  integration comes for free.

- **`futex`** (Linux) / **`__ulock`** (macOS) operates directly on a 32-bit word in memory, specifically
  `SPSCIndices::consumer_sleeping`, an `atomic<uint32_t>` already in the shared memory region. `FUTEX_WAIT` atomically
  checks that the word still equals the expected value before sleeping, closing the lost-wakeup window. `FUTEX_WAKE`
  costs one syscall from the producer with no fd to track. The timeout parameter (`WATCHDOG_TIMEOUT_US = 200 ms`) is
  passed directly; no timer fd is needed. The futex address is the `consumer_sleeping` atomic itself, so no extra data
  structure is required.

The lost-wakeup race (consumer observes empty), producer writes and calls WAKE before the consumer calls WAIT. It is
handled by the double-check pattern in `acquire_rx_blocking`: consumer stores `consumer_sleeping = 1`, issues a
`seq_cst` fence, re-reads the head, and only calls `FUTEX_WAIT` if the queue is still empty. If the producer wrote
between the store and the fence, the re-read catches it without sleeping.

ThreadSanitizer cannot follow the producer→consumer happens-before through `SYS_futex` directly (it is an opaque
syscall). TSan annotations `__tsan_acquire` / `__tsan_release` are placed around the `platform_wait` / `platform_wake`
calls so that TSan sees the correct edge.

## Decision

Use `SYS_futex` on Linux (`FUTEX_WAIT` with `200 ms` timeout) and `__ulock_wait` / `__ulock_wake` on macOS. The wait
word is `SPSCIndices::consumer_sleeping` in the shared SHM region. No eventfd, no additional file descriptor, no second
SCM_RIGHTS transfer.

## Consequences

**Positive**

- One syscall per wake from the producer (`FUTEX_WAKE`), not two.
- No fd to allocate, track, pass, or close. The wait address is part of the SHM header already transferred via
  `SCM_RIGHTS`.
- The 200 ms watchdog timeout bounds consumer sleep without a timer fd or SIGALRM.
- `epoll` integration is not needed; Tachyon's consumer model is blocking-per-bus, not multiplexed I/O.

**Negative**

- `SYS_futex` is Linux-only; `__ulock` is macOS-only and private API (no header, `extern "C"` declaration). Portability
  to other UNIX variants (FreeBSD, Solaris) would require a third branch or a fallback to `nanosleep` + spin.
- TSan annotations are required and must be maintained. Forgetting them when touching the futex paths causes false TSan
  positives in CI.
- The 200 ms watchdog is a spin-reset bound, not a dead-peer detector. A crashed producer leaves the consumer sleeping
  indefinitely between timeouts. Dead-peer detection via TSC heartbeat is deferred to v0.4.0.

**Neutral**

- `__ulock` is not documented by Apple but has been stable across macOS versions since 10.12. It is the same primitive
  used by `std::mutex` in libc++ on Apple platforms.
- The `TACHYON_ERR_INTERRUPTED` return code propagates `EINTR` from `FUTEX_WAIT` up through the C API and all language
  bindings, allowing signal handlers to interrupt blocking calls cleanly.
