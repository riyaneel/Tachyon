# ADR-004 — `TACHYON_MSG_ALIGNMENT = 64`

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

Every message in the ring buffer consists of a `MessageHeader` immediately followed by the payload. Both the header and
the next header after it must be aligned so that:

1. The header fields (`size`, `type_id`, `reserved_size`) can be read with a single non-tearing load on any supported
   architecture.
2. The payload pointer returned to the caller is aligned to at least 32 bytes, satisfying AVX2 load/store requirements
   without the caller having to add padding.
3. Two consecutive headers never share a cache line, so a producer writing the next header does not invalidate the cache
   line the consumer is currently reading.

The alignment value must be a compile-time constant and a power of two. Candidates:

- **32 bytes** — satisfies AVX2 alignment, but `sizeof(MessageHeader)` is at least 12 bytes (`size` + `type_id` +
  `reserved_size`, each 4 bytes). Padding to 32 bytes leaves 20 bytes of waste, and two headers can share a 64-byte
  cache line, creating producer/consumer false sharing on adjacent messages.
- **64 bytes** — matches the cache line width on all x86-64 and ARM64 microarchitectures in production use. Each header
  occupies exactly one cache line. `sizeof(MessageHeader)` is exactly 64 bytes (`3 * uint32_t = 12 bytes`, padded to
  64). The payload is 64-byte aligned. AVX-512 (64-byte) loads are satisfied.
- **128 bytes** — eliminates any possibility of producer/consumer sharing even on adjacent cache line prefetcher, but
  doubles the per-message overhead for small payloads. A 1-byte message costs 128 bytes of ring buffer space.

The alignment value is compiled into both the producer and the consumer and is validated during the handshake
(`TachyonHandshake.msg_alignment`). A mismatch returns `TACHYON_ERR_ABI_MISMATCH` before the first byte of data is
exchanged.

## Decision

`TACHYON_MSG_ALIGNMENT = 64`. It is a compile-time `#define` with a CMake override (`-DTACHYON_MSG_ALIGNMENT=N`),
validated to be a power of two ≥ 32 via `static_assert`. The value is embedded in `ArenaHeader` and `TachyonHandshake`
and checked at attach and connect time. Any change to the value constitutes a wire protocol break and requires a
`TACHYON_VERSION` bump.

## Consequences

**Positive**

- Each `MessageHeader` occupies exactly one 64-byte cache line. No adjacent-message false sharing between the producer
  writing a header and the consumer reading the previous one.
- Payload pointers are 64-byte aligned, satisfying AVX-512, AVX2, and NEON without the caller adding alignment padding.
- The overhead is fixed and predictable: every message costs at least 64 bytes of ring buffer space regardless of
  payload size. For a 0-byte payload, overhead is 100%. For a 1 KB payload, overhead is ~6%.

**Negative**

- High overhead ratio for tiny payloads (< 32 bytes). A stream of 8-byte ticks wastes 87.5% of ring buffer
  capacity to padding. Applications sending many tiny messages should batch at the application layer before calling
  `acquire_tx`.
- Changing `TACHYON_MSG_ALIGNMENT` is a breaking wire protocol change. Any operator running a custom value must
  recompile both producer and consumer simultaneously.

**Neutral**

- The `__builtin_prefetch` hints in `acquire_tx` and `acquire_rx_batch` assume 64-byte alignment; they remain correct
  for any value ≥ 64.
- The layout test in `tachyon-sys/src/lib.rs` (`assert_eq!(mem::size_of::<tachyon_msg_view_t>(), 32)`) is independent
  of `TACHYON_MSG_ALIGNMENT`; `tachyon_msg_view_t` is a view struct, not a ring buffer header.
