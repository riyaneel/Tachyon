import types
from typing import Optional, Type


class TachyonError(Exception):
    """Base exception for Tachyon errors"""
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


class TachyonBus:
    """Tachyon IPC Bus"""

    def listen(self, socket_path: str, capacity: int) -> None:
        """Formats and initializes a new IPC bus on the specified UNIX socket"""
        ...

    def connect(self, socket_path: str) -> None:
        """Connects to an existing IPC bus via UNIX socket descriptor"""
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
