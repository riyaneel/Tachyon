# Architecture Decision Records

This directory contains Architecture Decision Records (ADRs) for Tachyon.
Each ADR documents a significant design choice, the context that motivated it, and its consequences.

New ADRs use the template at the bottom of this file. Once accepted, an ADR is never deleted — superseded decisions are
marked **Superseded** and linked to the replacement.

---

## Index

| ADR                                             | Title                               | Status   | Date       |
|-------------------------------------------------|-------------------------------------|----------|------------|
| [ADR-001](ADR-001-memfd-vs-shm-open.md)         | `memfd_create` vs `shm_open`        | Accepted | 2026-03-30 |
| [ADR-002](ADR-002-spsc-vs-mpsc.md)              | SPSC strict vs MPSC                 | Accepted | 2026-03-30 |
| [ADR-003](ADR-003-futex-vs-eventfd.md)          | Futex vs eventfd for consumer sleep | Accepted | 2026-03-30 |
| [ADR-004](ADR-004-msg-alignment.md)             | `TACHYON_MSG_ALIGNMENT = 64`        | Accepted | 2026-03-30 |
| [ADR-005](ADR-005-scm-rights-vs-named-mmap.md)  | SCM_RIGHTS vs named shared memory   | Accepted | 2026-03-30 |
| [ADR-006](ADR-006-no-serialization-contract.md) | No-serialization contract           | Accepted | 2026-03-30 |

---

## Statuses

- **Accepted** — in effect, implemented in the codebase.
- **Superseded** — replaced by a later ADR; kept for history.
- **Deprecated** — no longer recommended; will be removed in a future major version.
- **Proposed** — under discussion, not yet merged.

---

## Template

```markdown
# ADR-NNN — Title

---

**Status:** Proposed | Accepted | Superseded by ADR-NNN | Deprecated  
**Date:** YYYY-MM-DD

---

## Context

What is the problem? What forces are at play? What constraints exist?
Be concrete: include benchmarks, kernel versions, and API limitations where relevant.

## Decision

What did we decide? One clear sentence, then the reasoning.

## Consequences

**Positive**

- …

**Negative**

- …

**Neutral**

- …
```
