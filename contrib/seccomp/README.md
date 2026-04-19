# Seccomp

Seccomp-BPF allowlist generators for Tachyon. Standalone C programs, no dependency on the Tachyon build system.
Both profiles target **C++ and Rust only.** See [Binding notes](../../SYSCALLS.md#binding-notes) before applying inside
any other runtime.

## Files

| File                    | Scope                                 |
|-------------------------|---------------------------------------|
| `tachyon_hotpath.bpf.c` | Hot path only. Apply after handshake. |
| `tachyon_full.bpf.c`    | Handshake + hot path + teardown.      |

## Build

Requires libseccomp (`libseccomp-dev` on Ubuntu/Debian, `libseccomp-devel` on Fedora).

```bash
# Via CMake
cmake --preset seccomp-bpf
cmake --build --preset seccomp-bpf

# Standalone
clang -Wall -Wextra -Werror tachyon_hotpath.bpf.c -o tachyon_hotpath -lseccomp
clang -Wall -Wextra -Werror tachyon_full.bpf.c -o tachyon_full -lseccomp
```

## Usage

```bash
# Binary BPF (load via prctl)
./tachyon_hotpath > tachyon_hotpath.bpf
./tachyon_full > tachyon_full.bpf

# Human readable
./tachyon_hotpath --pfc
./tachyon_full --pfc
```

Apply after handshake, before the hot path loop:

```c++
tachyon_bus_listen(path, capacity, &bus);

struct sock_fprog prog = { ... }; /* load tachyon_hotpath.bpf */
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
```

**WARNING:** These profiles are absolute (`SCMP_ACT_KILL_PROCESS`). They authorize *only* the syscalls required by
Tachyon internal lock-free mechanisms. If your consumer logic requires heap allocations (`brk`/`mmap`), disk I/O
(`write`), or threading (`clone`), you **must** modify the `.c` generator to include your application's specific
syscalls before generating the BPF blob.

## Scope

`tachyon_hotpath` covers: `futex`, `munmap`, `close`, `exit_group`, `rt_sigreturn`. Any syscall emitted by the
application outside Tachyon must be added by the caller.

`tachyon_full` adds all handshake syscalls. Only use it when the application emits no other syscalls between process
start and the first `tachyon_bus_listen()` / `tachyon_bus_connect()`.

Default action is `SCMP_ACT_KILL_PROCESS`. Change to `SCMP_ACT_ERRNO(EPERM)` for debugging.
