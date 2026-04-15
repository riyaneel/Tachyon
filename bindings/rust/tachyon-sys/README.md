# tachyon-sys

Low-level FFI bindings for the [Tachyon](https://github.com/riyaneel/Tachyon) IPC C core.

This crate exposes the raw `tachyon.h` C API via `bindgen`-generated bindings. It is not intended for direct use!
Use [`tachyon-ipc`](https://crates.io/crates/tachyon-ipc) instead.

## What this crate provides

- `tachyon_bus_t`, `tachyon_msg_view_t`: opaque and layout types
- `tachyon_bus_listen`, `tachyon_bus_connect`, `tachyon_bus_destroy`
- `tachyon_bus_set_numa_node`, `tachyon_bus_set_polling_mode`
- `tachyon_acquire_tx`, `tachyon_commit_tx`, `tachyon_rollback_tx`, `tachyon_flush`
- `tachyon_acquire_rx_blocking`, `tachyon_commit_rx`
- `tachyon_drain_batch`, `tachyon_commit_rx_batch`
- `tachyon_error_t`, `tachyon_state_t`: error and state enums

## Build

The C++ core (`arena.cpp`, `shm.cpp`, `tachyon_c.cpp`, `transport_uds.cpp`) is compiled at build time via the `cc`
crate. Requires GCC 14+ or Clang 17+.

When published to crates.io, the C++ sources are vendored under `vendor/core/`.
In a local checkout of the Tachyon monorepo, `build.rs` resolves `../../../core` instead.

## License

Apache 2.0
