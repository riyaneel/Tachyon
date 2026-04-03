# tachyon-top

A zero-allocation, lock-free observability CLI for Tachyon IPC.

`tachyon-top` provides real-time supervision of active queues. It maps the underlying `memfd` regions in strictly
`PROT_READ` mode, ensuring that the observer **never causes cache line invalidations** on the producer/consumer critical
path (MESI `Shared` state is preserved).

---

## Features

- **Hardware Empathy:** Uses raw TSC (Time Stamp Counter) ticks for timeout and heartbeat calculations, entirely
  avoiding expensive `clock_gettime` syscalls in the hot loop.
- **Zero-Allocation Rendering:** Decouples `/proc` scanning from FTXUI rendering using a lock-free triple-buffer state.
  Once running, it performs zero dynamic memory allocations.
- **JSON Export:** Machine-readable state dump for programmatic supervision and CI/CD pipelines.

---

## Usage

Run the interactive TUI. Depending on your system's `ptrace_scope`, discovering active buses via `/proc/*/fd` may
require `root` privileges or `CAP_SYS_PTRACE`:

```bash
sudo ./tachyon-top
```

### Keyboard Shortcuts

| Key                    | Action                                          |
|------------------------|-------------------------------------------------|
| `q` or `ESC`           | Quit                                            |
| `↑` / `↓` or `k` / `j` | Select a specific bus to view sparkline details |
| `r`                    | Cycle refresh interval (100ms / 500ms / 1000ms) |

---

## JSON Mode

Dump the current state of all active buses to `stdout` and exit immediately. Useful for CI smoke tests and external
metrics collectors.

```bash
./tachyon-top --json
```

*Example output:*

```json
[
  {
    "pid": 12345,
    "comm": "producer_proc",
    "state": "READY",
    "capacity": 1048576,
    "used_bytes": 4096,
    "fill_pct": 0.39,
    "msg_per_sec": 145000.5,
    "mb_per_sec": 56.2,
    "consumer_sleeping": false
  }
]
```
