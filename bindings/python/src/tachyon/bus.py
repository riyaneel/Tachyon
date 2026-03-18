from __future__ import annotations

import types
from contextlib import contextmanager
from typing import Iterator, Generator, Optional, Type
from . import _tachyon
from .message import Message


class Bus:
    __slots__ = ("_bus",)

    def __init__(self) -> None:
        """Private. Use listen() or connect() factories."""
        self._bus = _tachyon.TachyonBus()

    def __enter__(self) -> Bus:
        return self

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType],
    ) -> None:
        self._bus.destroy()

    @classmethod
    def listen(cls, socket_path: str, capacity: int) -> Bus:
        """Creates SHM arena and binds UNIX socket."""
        instance = cls()
        instance._bus.listen(socket_path=socket_path, capacity=capacity)
        return instance

    @classmethod
    def connect(cls, socket_path: str) -> Bus:
        """Attaches to existing SHM arena via UNIX socket."""
        instance = cls()
        instance._bus.connect(socket_path=socket_path)
        return instance

    def send(self, data: bytes, type_id: int = 0) -> None:
        """Blocking SPSC write. Copies payload, commits, and flushes."""
        size = len(data)
        with self._bus.acquire_tx(size) as tx:
            with memoryview(tx) as m:
                m[:size] = data
            tx.actual_size = size
            tx.type_id = type_id

        self._bus.flush()

    def __iter__(self) -> Iterator[Message]:
        """Blocking RX iterator. Yields copied Messages (bytes) to prevent Use-After-Commit."""
        while True:
            with self._bus.acquire_rx() as rx:
                with memoryview(rx) as m:
                    payload = m.tobytes()

                msg = Message(type_id=rx.type_id, size=rx.actual_size, data=payload)

            yield msg

    @contextmanager
    def send_zero_copy(self, size: int, type_id: int = 0) -> Generator[_tachyon.TxGuard, None, None]:
        """Zero-copy TX lock. Yields raw TxGuard. Caller must update actual_size. Auto-flushes on exit."""
        with self._bus.acquire_tx(size) as tx:
            tx.type_id = type_id
            yield tx
        self._bus.flush()

    def recv_zero_copy(self) -> _tachyon.RxGuard:
        """Zero-copy RX lock. Returns raw RxGuard. Caller must release memoryview before context exit."""
        return self._bus.acquire_rx()

    def drain_batch(self, max_msgs: int = 1024, spin_threshold: int = 10000) -> Any:
        """
        Blocks until at least 1 message is available, then drains up to `max_msgs` from the ring buffer.
        Returns a context manager yielding a sequence of messages.
        """
        return self._bus.drain_batch(max_msgs, spin_threshold)
