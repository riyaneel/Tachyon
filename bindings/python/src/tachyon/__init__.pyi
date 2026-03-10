class TachyonError(Exception):
    """Base exception for Tachyon errors"""
    pass

class TachyonBus:
    """Tachyon IPC Bus"""

    def listen(self, socket_path: str, capacity: int) -> None:
        """
        Formats and initializes a new IPC bus on the specified UNIX socket
        """
        ...

    def connect(self, socket_path: str) -> None:
        """
        Connects to an existing IPC bus via UNIX socket descriptor
        """
        ...

    def destroy(self) -> None:
        """
        Explicitly unmap shared memory and closes fds
        """
        ...

    def flush(self) -> None:
        """
        Forcefully flushes pending TX transactions to the consumer
        """
        ...