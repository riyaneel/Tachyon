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
    def type_id(self) -> int: ...

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

    def drain_batch(self, max_msgs: int = 1024, spin_threshold: int = 10000) -> Any:
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
