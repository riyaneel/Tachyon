import types
from typing import Any, Optional, Type


class TachyonError(Exception):
    """Base exception for Tachyon errors"""
    pass


class PeerDeadError(TachyonError):
    """Raised when the bus transitions to FatalError state (corrupted message header)."""
    pass


class TxGuard:
    """Tachyon TX Guard Context Manager"""
    actual_size: int
    type_id: int
    """Message type discriminator.

    Encoded as 16+16 bits since v0.4.0: bits [31:16] = route_id, bits [15:0] = msg_type.
    Set route_id=0 (or use make_type_id(0, x)) to preserve v0.3.x behavior.
    """

    def __enter__(self) -> "TxGuard": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType]
    ) -> bool: ...

    def __buffer__(self, flags: int) -> memoryview: ...


class RxGuard:
    """Tachyon RX Guard Context Manager"""

    @property
    def actual_size(self) -> int: ...

    @property
    def type_id(self) -> int: ...

    def __enter__(self) -> "RxGuard": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType]
    ) -> bool: ...

    def __buffer__(self, flags: int) -> memoryview: ...


class RxMsgView:
    @property
    def actual_size(self) -> int: ...

    @property
    def type_id(self) -> int:
        """Message type discriminator.

        Encoded as 16+16 bits since v0.4.0: bits [31:16] = route_id, bits [15:0] = msg_type.
        Use route_id() and msg_type() helpers to split.
        """
        ...

    def __dlpack__(self, stream: Any = None) -> Any: ...

    def __dlpack_device__(self) -> tuple[int, int]: ...

    def __buffer__(self, flags: int) -> memoryview: ...


class RxBatchGuard:
    def __enter__(self) -> 'RxBatchGuard': ...

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool: ...

    def __len__(self) -> int: ...

    def __getitem__(self, index: int) -> RxMsgView: ...


class TachyonBus:
    """Tachyon IPC Bus"""

    def listen(self, socket_path: str, capacity: int) -> None:
        """
        Creates a new shared memory ring buffer and listens for a consumer.
        This call blocks until a consumer connects via `TachyonBus.connect()`

        :raise KeyboardInterrupt: If interrupted by a signal while waiting.
        :raise RuntimeError: If the bus is already initialized.
        :raise SystemError: On SHM or OS failure.
        """
        ...

    def connect(self, socket_path: str) -> None:
        """
        Connects to an existing IPC bus via UNIX socket descriptor.

        :raise ConnectionError: If the producer was compiled with a different
            Tachyon version or TACHYON_MSG_ALIGNMENT value (ABI mismatch).
        :raise ConnectionError: If the UNIX socket is unreachable.
        :raise RuntimeError: If the bus is already initialized.
        """
        ...

    def destroy(self) -> None:
        """Explicitly unmap shared memory and closes fds"""
        ...

    def flush(self) -> None:
        """Forcefully flushes pending TX transactions to the consumer"""
        ...

    def acquire_tx(self, max_payload_size: int) -> TxGuard:
        """Acquires a TX lock on the arena for writing"""
        ...

    def acquire_rx(self, spin_threshold: int = 10000) -> RxGuard:
        """
        Acquires an RX lock on the arena for reading.
        Blocks until data is available, spinning up to `spin_threshold` times before sleeping.
        """
        ...

    def drain_batch(self, max_msgs: int = 1024, spin_threshold: int = 10000) -> RxBatchGuard:
        """
        Blocks until at least 1 message is available, then drains up to `max_msgs` from the ring buffer.
        Returns a context manager yielding a sequence of messages.
        """
        ...

    def set_numa_node(self, node_id: int) -> None:
        """
        Binds the shared memory backing this bus to a specific NUMA node.
        Uses MPOL_PREFERRED + MPOL_MF_MOVE. No-op on non-Linux platforms.

        :raise ValueError: node_id negative or >= 64.
        :raise OSError: mbind() failure.
        """
        ...

    def set_polling_mode(self, pure_spin: int) -> None:
        """
        Signals that the consumer will never sleep, skipping the futex wake check
        on every producer flush. pure_spin=1 enables, 0 restores hybrid mode.
        """
        ...


class RpcTxGuard:
    """Tachyon RPC TX Guard Context Manager.

    Wraps a zero-copy slot in arena_fwd (call side) or arena_rev (reply side).
    Fill the buffer via memoryview, set actual_size and msg_type, then exit the
    context to commit. On exception the slot is rolled back automatically.
    out_cid is populated after a successful call-side commit; always 0 for replies.
    """

    actual_size: int
    msg_type: int

    @property
    def out_cid(self) -> int:
        """Correlation ID assigned after commit (call side only). 0 before commit or for replies."""
        ...

    def __enter__(self) -> "RpcTxGuard": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType],
    ) -> bool: ...

    def __buffer__(self, flags: int) -> memoryview: ...


class RpcRxGuard:
    """Tachyon RPC RX Guard Context Manager.

    Wraps a zero-copy slot in arena_rev (caller wait side) or arena_fwd (callee
    serve side). Read via memoryview, then exit the context to release the slot.
    correlation_id must be read before or during the context — it is invalid after exit.
    """

    @property
    def actual_size(self) -> int: ...

    @property
    def type_id(self) -> int: ...

    @property
    def correlation_id(self) -> int:
        """Correlation ID of this message. Use to dispatch replies on the callee side."""
        ...

    def __enter__(self) -> "RpcRxGuard": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType],
    ) -> bool: ...

    def __buffer__(self, flags: int) -> memoryview: ...


class TachyonRpcBus:
    """Tachyon RPC Bus — low-level C extension type.

    Prefer the high-level RpcBus wrapper. Use this directly only when you need
    raw guard access without the Python-layer copies.
    """

    def listen(self, socket_path: str, cap_fwd: int, cap_rev: int) -> None:
        """
        Creates two SHM arenas (fwd + rev) and blocks until a connector arrives.

        :raise KeyboardInterrupt: Interrupted by signal while waiting.
        :raise RuntimeError: Bus already initialized.
        :raise SystemError: SHM or OS failure.
        """
        ...

    def connect(self, socket_path: str) -> None:
        """
        Attaches to existing SHM arenas via UNIX socket.

        :raise ConnectionError: ABI mismatch (version or flags mismatch).
        :raise ConnectionError: UNIX socket unreachable.
        :raise RuntimeError: Bus already initialized.
        """
        ...

    def destroy(self) -> None:
        """Explicitly unmap both SHM arenas and close fds."""
        ...

    def acquire_call(self, max_payload_size: int) -> RpcTxGuard:
        """
        Acquires a zero-copy TX slot in arena_fwd (caller → callee).
        Returns an RpcTxGuard. Set actual_size and msg_type before exiting the context.
        out_cid is populated after successful __exit__.

        :raise PeerDeadError: arena_fwd in FatalError state.
        :raise TachyonError: Ring buffer full.
        :raise ValueError: max_payload_size <= 0.
        """
        ...

    def acquire_reply(self, correlation_id: int, max_payload_size: int) -> RpcTxGuard:
        """
        Acquires a zero-copy TX slot in arena_rev (callee → caller).
        Returns an RpcTxGuard with is_reply=1. correlation_id must be non-zero.

        :raise PeerDeadError: arena_rev in FatalError state.
        :raise TachyonError: Ring buffer full.
        :raise ValueError: correlation_id == 0 or max_payload_size <= 0.
        """
        ...

    def wait(self, correlation_id: int, spin_threshold: int = 10000) -> RpcRxGuard:
        """
        Blocks until the response matching correlation_id arrives in arena_rev.
        Returns an RpcRxGuard. A cid mismatch triggers FatalError on arena_rev.

        :raise PeerDeadError: Cid mismatch or arena_rev FatalError.
        :raise KeyboardInterrupt: Interrupted by signal.
        """
        ...

    def serve(self, spin_threshold: int = 10000) -> RpcRxGuard:
        """
        Blocks until a request arrives in arena_fwd.
        Returns an RpcRxGuard. Read correlation_id from the guard for the reply.

        :raise PeerDeadError: arena_fwd in FatalError state.
        :raise KeyboardInterrupt: Interrupted by signal.
        """
        ...

    def set_polling_mode(self, pure_spin: int) -> None:
        """
        Signals that both consumers (fwd + rev) will never sleep.
        pure_spin=1 enables, 0 restores hybrid mode.
        """
        ...
