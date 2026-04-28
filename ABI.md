# ABI Reference

## Wire Protocol

### Handshake (v0x04)

Sent once over UDS via `SCM_RIGHTS`. Socket discarded after exchange.

```c++
struct TachyonHandshake {
    uint32_t magic;         /* 0x54414348 ("TACH") */
    uint32_t version;       /* currently 0x04 */
    uint32_t capacity_fwd;  /* ring size caller -> callee, power of two */
    uint32_t shm_size_fwd;  /* sizeof(MemoryLayout) + capacity_fwd */
    uint32_t capacity_rev;  /* ring size callee -> caller; 0 for SPSC */
    uint32_t shm_size_rev;  /* sizeof(MemoryLayout) + capacity_rev; 0 for SPSC */
    uint32_t msg_alignment; /* TACHYON_MSG_ALIGNMENT compiled into producer */
    uint32_t flags;         /* 0 = SPSC */
};
```

Native endian. Flat `iovec`, no framing. One `sendmsg` transfers 1 fd (SPSC) or 2 fds (RPC).

### Validation at `connect()`

| Field           | Condition                         | Failure                    |
|-----------------|-----------------------------------|----------------------------|
| `magic`         | `== 0x54414348`                   | `TACHYON_ERR_ABI_MISMATCH` |
| `version`       | `== TACHYON_VERSION` (`0x04`)     | `TACHYON_ERR_ABI_MISMATCH` |
| `msg_alignment` | `== TACHYON_MSG_ALIGNMENT` (`64`) | `TACHYON_ERR_ABI_MISMATCH` |
| `flags`         | `== 0` for SPSC connector         | `TACHYON_ERR_ABI_MISMATCH` |

Secondary validation in `Arena::attach()`: re-checks `magic`, `msg_alignment`, capacity power-of-two.

### MessageHeader (v0x04)

```c++
struct alignas(TACHYON_MSG_ALIGNMENT) MessageHeader {
    uint32_t size;
    uint32_t type_id; /* bits [31:16] = route_id, bits [15:0] = msg_type */
    uint32_t reserved_size;
    /* 4 bytes implicit padding (compiler, uint64_t alignment) */
    uint64_t correlation_id; /* 0 = SPSC (no correlation); >= 1 = RPC request/reply */
    uint8_t padding_[TACHYON_MSG_ALIGNMENT - (sizeof(uint32_t) * 4) - sizeof(uint64_t)];
};
```

`correlation_id = 0` is the SPSC sentinel. RPC counters start at 1.

### Bump Policy

Mandatory bump on any of:

- Field added, removed, or reordered in `TachyonHandshake`
- Field added, removed, or reordered in `MemoryLayout`, `ArenaHeader`, or `SPSCIndices`
- `MessageHeader` layout change (size, field order, sentinel values)
- `SKIP_MARKER` value change (`0xFFFFFFFF`)
- `TACHYON_MAGIC` change

No bump required for:

- New `tachyon_error_t` values
- New functions in `tachyon.h`
- `WATCHDOG_TIMEOUT_US` or `BATCH_SIZE` changes
- Language binding changes

### Version History

| Version | Change                                                                                                                                       |
|---------|----------------------------------------------------------------------------------------------------------------------------------------------|
| `0x01`  | Initial wire format                                                                                                                          |
| `0x02`  | `msg_alignment` added to `TachyonHandshake` and `ArenaHeader`                                                                                |
| `0x03`  | `type_id` encoding: bits [31:16] = route_id (reserved for RPC), bits [15:0] = msg_type.                                                      |
| `0x04`  | `TachyonHandshake` extended: `capacity_fwd/rev`, `shm_size_fwd/rev`, `flags`. `MessageHeader` extended: `correlation_id` added at offset 16. |

---

## C API Stability (from v0.1.0)

- Existing signatures: frozen
- Existing enum numeric values: frozen
- Additions: allowed without version bump
- `tachyon_bus_set_polling_mode` added in v0.3.0, sets `consumer_sleeping` to `CONSUMER_PURE_SPIN (2)`, disabling futex
  wake checks on the producer flush path. No wire format change.
- Removal or signature change: requires a major version bump.
- `TACHYON_TYPE_ID`, `TACHYON_ROUTE_ID`, `TACHYON_MSG_TYPE` macros added in v0.4.0.

Visibility: `TACHYON_ABI` on all exported symbols. Internals: `-fvisibility=hidden`.

---

## Known Limitations

- Endianness: assumed identical between producer and consumer. Not detected.
- Version check is strict equality. No forward compatibility.
- Heartbeat granularity: `producer_heartbeat` and `consumer_heartbeat` in `SPSCIndices` are updated at batch
  boundaries (every 32 commits), not on every flush. `tachyon-top` displays stale heartbeat ages on low-throughput
  buses. No IPC correctness impact.
