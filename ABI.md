# ABI Reference

## Wire Protocol

### Handshake (v0x02)

Sent once over UDS via `SCM_RIGHTS`. Socket discarded after exchange.

```c
struct TachyonHandshake {
    uint32_t magic;         /* 0x54414348 — "TACH" */
    uint32_t version;       /* currently 0x02 */
    uint32_t capacity;      /* ring size in bytes, power of two */
    uint32_t shm_size;      /* sizeof(MemoryLayout) + capacity */
    uint32_t msg_alignment; /* TACHYON_MSG_ALIGNMENT compiled into producer */
};
```

Native endian. Flat `iovec`, no framing.

### Validation at `connect()`

| Field           | Condition                         | Failure              |
|-----------------|-----------------------------------|----------------------|
| `magic`         | `== 0x54414348`                   | `TACHYON_ERR_SYSTEM` |
| `version`       | `== TACHYON_VERSION` (`0x02`)     | `TACHYON_ERR_SYSTEM` |
| `msg_alignment` | `== TACHYON_MSG_ALIGNMENT` (`64`) | `TACHYON_ERR_SYSTEM` |

Secondary validation in `Arena::attach()`: re-checks `magic`, `msg_alignment`, capacity power-of-two.

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

| Version | Change                                                        |
|---------|---------------------------------------------------------------|
| `0x01`  | Initial wire format                                           |
| `0x02`  | `msg_alignment` added to `TachyonHandshake` and `ArenaHeader` |

---

## C API Stability (from v0.1.0)

- Existing signatures: frozen
- Existing enum numeric values: frozen
- Additions: allowed without version bump
- Removal or signature change: requires major version bump

Visibility: `TACHYON_ABI` on all exported symbols. Internals: `-fvisibility=hidden`.

---

## Known Limitations

- `TACHYON_ERR_SYSTEM` is returned on handshake mismatch — maps to `OSError` in Python and reads `errno`, which is
  misleading. A dedicated `TACHYON_ERR_ABI_MISMATCH` is planned.
- Endianness: assumed identical between producer and consumer. Not detected.
- Version check is strict equality. No forward compatibility.
