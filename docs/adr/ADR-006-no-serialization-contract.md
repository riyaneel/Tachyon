# ADR-006 — No-serialization contract

---

**Status:** Accepted  
**Date:** 2026-03-30

---

## Context

IPC libraries typically provide a serialization layer: Protocol Buffers, MessagePack, FlatBuffers, Cap'n Proto, or a
custom framing protocol. The serialization layer handles type safety, schema evolution, and cross-language compatibility
at the cost of an encode/decode step on every message.

Tachyon's primary use cases — ML inference pipelines, trading data feeds, audio/video inter-process — share a common
property: both ends are controlled by the same operator, compiled from the same source, and exchange data at RAM speed
where any per-message allocation or copy is measurable overhead. In these cases, imposing a serialization layer inside
the IPC primitive would be a mistake because:

1. **The operator already has a schema.** A `MarketTick` struct, a `float32[256]` feature vector, or a fixed audio frame
   has a layout defined at compile time. Re-encoding it into a serialization format just to decode it immediately on the
   other side wastes CPU.
2. **Zero-copy is only possible with raw bytes.** DLPack, Python `memoryview`, and Rust slice borrows all expose the SHM
   payload pointer directly to the consumer. Any serialization layer that writes into a staging buffer breaks zero-copy.
3. **Schema evolution is an application concern at Tachyon's layer.** A versioned `type_id` discriminant (`uint32_t`) is
   sufficient for the transport to route messages; the application decides what each `type_id` means and how to evolve
   it.

The alternative — providing an optional serialization plugin system — would add complexity to the hot path (vtable
dispatch, conditional branching) and to the binding API surface (a new generic parameter or codec registration call in
every language). The cost/benefit does not justify it at Tachyon's abstraction level.

## Decision

Tachyon transports raw bytes. The core carries no schema, no encoding, and no framing beyond `MessageHeader` (`size`,
`type_id`, `reserved_size`). `type_id` is an opaque `uint32_t`; its meaning is defined entirely by the application. All
language bindings expose the payload as the native zero-copy primitive for that language: `std::byte *` in C++, `&[u8]`
in Rust, `memoryview` + DLPack in Python, `unsafe.Slice` in Go. Serialization is the caller's responsibility.

## Consequences

**Positive**

- Zero-copy is unconditional. The payload pointer returned by `acquire_rx` points directly into the SHM ring buffer with
  no intermediate buffer. PyTorch, JAX, and NumPy can consume it via DLPack without a copy.
- No per-message allocation. `acquire_tx` returns a pointer into the ring; `commit_tx` writes a header. The hot path is
  pointer arithmetic and a `memcpy` if the caller chooses to copy.
- The binding API is minimal and uniform: `acquire_tx(size) → ptr`, `commit_tx(actual, type_id)`. Every language binding
  expresses the same contract in its idiomatic zero-copy primitive.
- No dependency on a serialization library. The core has no transitive dependencies beyond the C++ standard library and
  POSIX.

**Negative**

- Cross-language struct compatibility is the caller's responsibility. A Go consumer reading a C++ struct must match
  field offsets exactly; there is no schema to fall back on. This is documented in `INTEGRATION.md` and the
  cross-language examples.
- Schema evolution (adding a field to a struct) requires the operator to version `type_id` and handle both old and new
  layouts explicitly. Tachyon provides no migration helpers.
- Debugging raw bytes is harder than debugging a self-describing format. Operators building observability tools must
  implement their own payload inspection.

**Neutral**

- `tachyon-top` (v0.3.0) will surface per-bus message counts, sizes, and `type_id` distributions via a
  `PROT_READ` monitor, without needing to decode payloads.
- Applications that do need schema evolution can embed a FlatBuffers or Cap'n Proto payload inside a Tachyon message.
  `type_id` can carry a schema discriminant. This is a valid pattern and does not require any changes to Tachyon.
