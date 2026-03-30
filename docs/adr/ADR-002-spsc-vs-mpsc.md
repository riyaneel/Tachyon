# ADR-002 — SPSC strict vs MPSC

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

The ring buffer at the core of Tachyon must serve at least two concurrent roles: a single producer writing market data
or feature vectors, and a single consumer reading them. The question is whether to support multiple concurrent producers
or consumers within a single ring buffer instance.

- **MPSC (multiple-producer single-consumer)** requires a compare-and-swap loop on the head pointer so that two
  producers can each reserve a contiguous slot without overlap. Even with a single producer at runtime, the CAS
  instruction is on the critical path on every`acquire_tx`. Under contention — two producers racing — the CAS loop
  spins, degrading to unpredictable latency. MPSC also requires head and tail to live in separate cache lines that both
  sides write, and the per-message metadata must encode which slots are committed vs pending, adding 8–16 bytes of
  overhead per message.

- **SPSC (single-producer single-consumer)** needs only two atomics: `head` (writer-owned) and `tail` (reader-owned).
  The producer reads `tail` to check for space; the consumer reads `head` to check for data. There are no CAS loops, no
  slot-state bytes, and no false sharing between producer and consumer cache lines (guaranteed by 128-byte `alignas`
  padding on the `SPSCIndices` struct). The hot path in `acquire_tx` and `acquire_rx` is a load, a subtraction, and a
  conditional branch — no atomics on the write path until the batch flush.

The practical consequence of choosing SPSC is that fan-in (N producers to 1 consumer) requires N independent ring
buffers and a multiplexing layer in the application. This is not a limitation for the primary use cases (ML inference
pipeline, trading feed, audio DSP), which are all strictly single-producer.

`tachyon_bus_t` adds a TAS (`atomic_flag`) producer lock and a separate consumer lock at the C API level to prevent
accidental concurrent calls from different threads on the same bus. This is not multi-producer support — it is a
bug-catcher. The locks are uncontended on the correct usage path and add ~0 ns to p50 RTT.

## Decision

Use SPSC. The ring buffer itself has no concurrency mechanism beyond the two head/tail atomics. The TAS locks in
`tachyon_bus_t` prevent API misuse but do not enable multi-producer semantics. Fan-in is documented as an application
concern: use N buses.

## Consequences

**Positive**

- Hot path contains zero CAS instructions. `acquire_tx` and `acquire_rx` are a load, a mask, and a branch. This is
  measurable: p50 intra-process RTT is 88–107 ns.
- No slot-state bytes in `MessageHeader`; the header is purely `size / type_id / reserved_size`.
- False sharing between producer and consumer is structurally impossible: head and tail each occupy their own 128-byte
  cache line.
- Simpler correctness argument: the memory model proof is two-sided acquire/release, not a multi-party protocol.

**Negative**

- Fan-in requires N buses. A star topology (one consumer, N producers) needs application-level multiplexing — planned as
  v0.5.0.
- The TAS producer lock means two threads sharing a `Bus` object will serialize rather than parallelise; this is the
  correct behaviour but may surprise users who expect lock-free multi-threading at the binding level.

**Neutral**

- RPC patterns (bidirectional channel) compose two SPSC buses — one per direction. The latency overhead is one extra
  ring buffer lookup per round trip. Planned as v0.5.0.
- MPSC as a first-class primitive remains possible in a future major version without breaking the current wire protocol,
  since `MessageHeader` has no producer-identity field today.
