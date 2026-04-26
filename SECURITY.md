# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.4.x   | ✓         |
| 0.3.5   | ✗         |
| < 0.3   | ✗         |

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Report vulnerabilities privately
via [GitHub Security Advisories](https://github.com/riyaneel/Tachyon/security/advisories/new).

Include:

- A description of the vulnerability and its potential impact
- Steps to reproduce or a minimal proof of concept
- Affected versions

You will receive an acknowledgement within 72 hours. We aim to release a fix within 14 days for critical issues.

## Scope

Tachyon is a same-machine IPC library. The threat model assumes that both the producer and consumer processes are
trusted and run under the same user or a cooperating user on the same host. Cross-machine transport and multi-tenant
isolation are explicitly out of scope.

Known limitations:

- **No encryption.** The shared memory region is readable by any process with access to the `memfd` file descriptor. Do
  not use Tachyon to communicate sensitive data between mutually untrusting processes.
- **No authentication.** The UDS handshake does not authenticate the connecting process beyond filesystem permissions on
  the socket path.

## Artifact verification

All release artifacts are signed with cosign and carry a SLSA Build Level 2 provenance attestation.

## Syscall containment

Tachyon's post-handshake hot path emits a single syscall type (`futex` on Linux, `__ulock` on macOS). The full syscall
footprint per phase is documented in [`SYSCALLS.md`](./SYSCALLS.md).

For C++ and Rust deployments that require seccomp-BPF containment, pre-built allowlist generators are available in
[`contrib/seccomp/`](./contrib/seccomp). Do not apply them from within a polyglot runtime (Go, Python, Java, Node.js).
