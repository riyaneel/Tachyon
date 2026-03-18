import types
from typing import Iterator, Generator, Optional, Type
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
__version__ = "0.1.0"

class PeerDeadError:
    """Raised when the peer process is dead or unresponsive."""
    pass

class TachyonError(Exception):
    """Base Tachyon IPC exception."""
    pass


TxGuard = _tachyon.TxGuard
RxGuard = _tachyon.RxGuard


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
