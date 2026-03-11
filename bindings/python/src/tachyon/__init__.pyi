import types
from typing import Iterator, Generator, Optional, Type

__all__ = ["Bus", "Message", "TachyonError"]
__version__ = "0.1.0"


class TachyonError(Exception):
    """Base Tachyon IPC exception."""
    pass


class TxGuard:
    """Zero-copy TX Context Manager."""
    actual_size: int
    type_id: int

    def __enter__(self) -> "TxGuard": ...

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType]
    ) -> bool: ...


class RxGuard:
    """Zero-copy RX Context Manager."""

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
