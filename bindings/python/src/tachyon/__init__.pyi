import types
from typing import Iterator, Generator, Optional, Type, Any
from . import _tachyon

__all__ = [
    "TachyonBus",
    "TachyonError",
    "PeerDeadError",
    "TxGuard",
    "RxGuard",
    "RxBatchGuard",
    "RxMsgView",
    "Bus",
    "Message",
]
__version__ = "0.2.0"


class TachyonError(Exception):
    """Base Tachyon IPC exception."""
    pass


class PeerDeadError(TachyonError):
    """Raised when the bus detects a corrupted message header (FatalError state)."""
    pass


TachyonBus = _tachyon.TachyonBus
TxGuard = _tachyon.TxGuard
RxGuard = _tachyon.RxGuard
RxBatchGuard = _tachyon.RxBatchGuard
RxMsgView = _tachyon.RxMsgView


class Message:
    """Tachyon Message Dataclass."""
    type_id: int
    size: int
    data: bytes | memoryview


class Bus:
    """Tachyon IPC High-Level API."""

    def __init__(self) -> None:
        """Private. Use listen() or connect()."""
        ...

    @classmethod
    def listen(cls, socket_path: str, capacity: int) -> "Bus":
        """Creates SHM arena and binds UNIX socket."""
        ...

    @classmethod
    def connect(cls, socket_path: str) -> "Bus":
        """Attaches to existing SHM arena via UNIX socket."""
        ...

    def __enter__(self) -> "Bus": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType]
    ) -> None:
        """Unmaps SHM and closes FDs."""
        ...

    def set_numa_node(self, node_id: int) -> None:
        """
        Binds the shared memory backing this bus to a specific NUMA node.

        Uses MPOL_PREFERRED + MPOL_MF_MOVE. Call immediately after listen()/connect().
        No-op on non-Linux platforms.

        :param node_id: NUMA node index (0-based, 0–63).
        :raise OSError: mbind() failure (invalid node or missing CAP_SYS_NICE).
        :raise ValueError: node_id negative or >= 64.
        """
        ...

    def set_polling_mode(self, pure_spin: int) -> None:
        """
        Signals that the consumer will never sleep, skipping the futex wake check
        on every producer flush.

        When pure_spin=1, the producer omits the atomic_thread_fence(seq_cst) and
        the consumer_sleeping load on every flush_tx. Use only when the consumer
        thread is dedicated and SCHED_FIFO — if it ever parks, the producer will
        not wake it.

        Call immediately after listen()/connect(), before the first message.

        :param pure_spin: 1 to enable pure-spin mode, 0 to restore hybrid mode.
        """
        ...

    def send(self, data: bytes, type_id: int = 0) -> None:
        """Blocking SPSC write. Copies payload, commits, and flushes."""
        ...

    def __iter__(self) -> Iterator[Message]:
        """Blocking RX iterator. Yields copied Messages (bytes) to prevent Use-After-Commit."""
        ...

    def send_zero_copy(self, size: int, type_id: int = 0) -> Generator[TxGuard, None, None]:
        """Zero-copy TX lock. Yields raw TxGuard. Caller must update actual_size. Auto-flushes on exit."""
        ...

    def recv_zero_copy(self) -> RxGuard:
        """Zero-copy RX lock. Returns raw RxGuard. Caller must release memoryview before context exit."""
        ...

    def drain_batch(self, max_msgs: int = 1024, spin_threshold: int = 10000) -> RxBatchGuard:
        """Batch RX. Blocks until ≥1 message, drains up to max_msgs."""
        ...
