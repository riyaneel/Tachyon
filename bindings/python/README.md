# tachyon-ipc (Python)

Python bindings for [Tachyon](https://github.com/riyaneel/tachyon), a bare-metal lock-free IPC. SPSC ring buffer over
POSIX shared memory with sub-100 ns p50 RTT.

The C++ core is exposed via a CPython extension (`_tachyon.so`). No subprocess, no serialization, no broker.

---

## Requirements

| Component | Minimum                                   |
|-----------|-------------------------------------------|
| OS        | Linux 5.10+ (primary), macOS 13+ (tier-2) |
| Python    | 3.11+                                     |
| Compiler  | GCC 14+ or Clang 17+ (build time only)    |

## Install

```bash
pip install tachyon-ipc
```

## Quickstart

The consumer must start first.

```python
# consumer
import tachyon

with tachyon.Bus.listen("/tmp/demo.sock", 1 << 16) as bus:
	for msg in bus:
		print(f"received {msg.size} bytes type_id={msg.type_id}")
		break
```

```python
# producer
import tachyon

with tachyon.Bus.connect("/tmp/demo.sock") as bus:
	bus.send(b"hello tachyon", type_id=1)
```

`Bus` is a context manager. `__exit__` destroys the bus and unmaps shared memory.

## API

### SPSC

`Bus.listen(socket_path, capacity)` creates an SHM arena and blocks until one producer connects.
`capacity` must be a strictly positive power of two.

`Bus.connect(socket_path)` attaches to an existing arena.

`bus.send(data, type_id=0)` copies `data` into the ring buffer, commits, and flushes.

`for msg in bus` blocks until each message is available. Each `msg` is a `Message(type_id, size, data)` with a
heap-owned `bytes` payload safe to retain indefinitely.

`bus.drain_batch(max_msgs, spin_threshold)` drains up to `max_msgs` in a single C crossing.

### Zero-copy TX

```python
with bus.send_zero_copy(size=64, type_id=7) as tx:
	with memoryview(tx) as m:
		m[:len(payload)] = payload
	tx.actual_size = len(payload)
# auto-flushes on context exit
```

### Zero-copy RX

```python
with bus.recv_zero_copy() as rx:
	with memoryview(rx) as m:
		snapshot = m.tobytes()  # copy before context exit
# slot committed on __exit__
```

## RPC

### Low-level

```python
# callee
with tachyon.RpcBus.listen("/tmp/rpc.sock", 1 << 16, 1 << 16) as callee:
	with callee.serve() as rx:
		cid = rx.correlation_id
		with memoryview(rx) as m:
			reply = m.tobytes()
	callee.reply(cid, reply, msg_type=2)

# caller
with tachyon.RpcBus.connect("/tmp/rpc.sock") as caller:
	cid = caller.call(b"ping", msg_type=1)
	with caller.wait(cid) as rx:
		with memoryview(rx) as m:
			print(m.tobytes())
```

`serve()` must be exited (committed) before `reply()` to avoid holding both arena slots simultaneously.

### Decorator API

```python
import tachyon
from tachyon import tachyon_rpc, RpcDispatcher


@tachyon_rpc(msg_type=1)
def echo(payload: memoryview) -> bytes:
	return payload.tobytes()


dispatcher = RpcDispatcher()
dispatcher.register(echo)

# callee
with tachyon.RpcBus.listen("/tmp/rpc.sock", 1 << 16, 1 << 16) as callee:
	dispatcher.serve_forever(callee)

# caller
with tachyon.RpcBus.connect("/tmp/rpc.sock") as caller:
	response = echo.call(caller, b"hello")
	print(response)
```

`@tachyon_rpc(msg_type)` binds a `memoryview -> bytes` function to a message type.
The handler receives a zero-copy `memoryview` valid only for the duration of the call. Call `.tobytes()` to retain.

`RpcDispatcher.serve_forever(bus)` runs the callee loop until `KeyboardInterrupt` or `PeerDeadError`.
If no handler is registered for a received `msg_type`, a `MSG_TYPE_ERROR` reply is sent to the caller before raising
locally. The caller is never left blocked.

`RpcEndpoint.call(bus, payload, on_response=None)` is the caller-side convenience wrapper. If `on_response` is
provided, the response `memoryview` is passed directly (zero-copy). If `None`, returns `bytes`.

### Error sentinel

`MSG_TYPE_ERROR = 0xFFFF`. When `serve_once` catches an unhandled msg_type or a handler exception, it sends a 2-byte
error payload `struct.pack("!H", unhandled_msg_type)` with `msg_type=0xFFFF`. Callers can detect this with
`_decode_error(payload)`.

## Batch RX

```python
with bus.drain_batch(max_msgs=64) as batch:
	for msg in batch:
		print(msg.type_id, msg.actual_size)
# all slots committed on __exit__
```

## Type ID encoding

```python
from tachyon import make_type_id, route_id, msg_type

type_id = make_type_id(0, 42)  # == 42
route_id(type_id)  # 0
msg_type(type_id)  # 42
```

`route_id >= 1` is reserved for RPC.

## Error handling

```python
from tachyon import TachyonError, PeerDeadError

try:
	with tachyon.Bus.connect("/tmp/demo.sock") as bus:
		bus.send(b"ping")
except PeerDeadError:
	# Bus entered FATAL_ERROR: corrupted header. Destroy immediately.
	pass
except TachyonError as e:
	print(e)
```

## Thread safety

`Bus` and `RpcBus` are not thread-safe. Each direction must be used from at most one thread. Tachyon is SPSC, not MPSC.

Blocking calls (`listen`, `for msg in bus`, `serve`) park the calling OS thread.
Do not call them from a thread that holds the GIL and must service other Python tasks.
Release the GIL before blocking C calls: the CPython extension calls `Py_BEGIN_ALLOW_THREADS` internally.

## NUMA binding

```python
with tachyon.Bus.listen(path, 1 << 20) as bus:
	bus.set_numa_node(0)  # call before first message
```

No-op on macOS.

## API surface

| Symbol                         | Description                                                        |
|--------------------------------|--------------------------------------------------------------------|
| `Bus`                          | SPSC bus. Context manager.                                         |
| `RpcBus`                       | Bidirectional RPC bus. Context manager.                            |
| `RpcDispatcher`                | Routes incoming `msg_type` to registered `RpcEndpoint` handlers.   |
| `RpcEndpoint` / `@tachyon_rpc` | Decorator binding a `memoryview -> bytes` handler to a `msg_type`. |
| `TxGuard`                      | Zero-copy TX slot (via `send_zero_copy`).                          |
| `RxGuard`                      | Zero-copy RX slot (via `recv_zero_copy` or `serve`).               |
| `RxBatchGuard`                 | Batch of RX slots (via `drain_batch`).                             |
| `Message`                      | Heap-owned message: `type_id`, `size`, `data`.                     |
| `TachyonError`                 | Base exception for all native errors.                              |
| `PeerDeadError`                | Bus entered `TACHYON_STATE_FATAL_ERROR`.                           |
| `MSG_TYPE_ERROR`               | `0xFFFF`. Sentinel for dispatcher error replies.                   |

## Limitations

Linux is the primary platform. `set_numa_node`, futex sleep, and `memfd_create` are Linux-specific.
SPSC only. No peer crash detection.

## License

Apache 2.0
