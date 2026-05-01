from __future__ import annotations

import types
from typing import Optional, Type

from . import _tachyon


class RpcBus:
    __slots__ = ("_rpc",)

    def __init__(self) -> None:
        """Private. Use rpc_listen() or rpc_connect() factories."""
        self._rpc = _tachyon.TachyonRpcBus()

    def __enter__(self) -> RpcBus:
        return self

    def __exit__(
            self,
            exc_type: Optional[Type[BaseException]],
            exc_val: Optional[BaseException],
            exc_tb: Optional[types.TracebackType],
    ) -> None:
        self._rpc.destroy()

    @classmethod
    def rpc_listen(cls, socket_path: str, cap_fwd: int, cap_rev: int) -> RpcBus:
        """Creates two SHM arenas (fwd + rev) and binds UNIX socket."""
        instance = cls()
        instance._rpc.listen(socket_path=socket_path, cap_fwd=cap_fwd, cap_rev=cap_rev)
        return instance

    @classmethod
    def rpc_connect(cls, socket_path: str) -> RpcBus:
        """Attaches to existing SHM arenas via UNIX socket."""
        instance = cls()
        instance._rpc.connect(socket_path=socket_path)
        return instance

    def set_polling_mode(self, pure_spin: int) -> None:
        """
        Signals that both consumers (fwd + rev arenas) will never sleep.

        When pure_spin=1, producers skip the futex wake check on every flush_tx.
        Use only when both sides run on dedicated threads at SCHED_FIFO.

        :param pure_spin: 1 to enable pure-spin mode, 0 to restore hybrid mode.
        """
        self._rpc.set_polling_mode(pure_spin=pure_spin)

    def call(
            self,
            payload: bytes,
            msg_type: int,
            max_payload_size: Optional[int] = None,
            spin_threshold: int = 10000,
    ) -> int:
        """
        Zero-copy write of a request into arena_fwd.
        Returns the assigned correlation_id.

        :param payload: Request bytes.
        :param msg_type: Application-level message type.
        :param max_payload_size: Reserve size. Defaults to len(payload).
        :param spin_threshold: Spin count before futex sleep on acquire.
        :return: correlation_id assigned to this call.
        """
        size = len(payload)
        reserve = max_payload_size if max_payload_size is not None else size

        with self._rpc.acquire_call(max_payload_size=reserve) as tx:
            with memoryview(tx) as m:
                m[:size] = payload
            tx.actual_size = size
            tx.msg_type = msg_type

        return tx.out_cid

    def call_zero_copy(self, max_payload_size: int, msg_type: int) -> _tachyon.RpcTxGuard:
        """
        Zero-copy TX call guard. Caller fills the buffer, sets actual_size and
        msg_type, then exits the context. Access out_cid after context exit.

        :param max_payload_size: Maximum bytes to reserve in arena_fwd.
        :param msg_type: Application-level message type (written to tx.msg_type).
        :return: RpcTxGuard context manager.
        """
        tx = self._rpc.acquire_call(max_payload_size=max_payload_size)
        tx.msg_type = msg_type
        return tx

    def wait(
            self,
            correlation_id: int,
            spin_threshold: int = 10000,
    ) -> _tachyon.RpcRxGuard:
        """
        Blocks until the response matching correlation_id arrives in arena_rev.
        Returns an RpcRxGuard context manager. Caller must exit it to release the slot.

        FatalError is raised (via PeerDeadError) if a cid mismatch is detected.

        :param correlation_id: The cid returned by call().
        :param spin_threshold: Spin count before futex sleep.
        :return: RpcRxGuard exposing the response buffer.
        """
        return self._rpc.wait(correlation_id=correlation_id, spin_threshold=spin_threshold)

    def serve(self, spin_threshold: int = 10000) -> _tachyon.RpcRxGuard:
        """
        Blocks until a request arrives in arena_fwd.
        Returns an RpcRxGuard context manager. Read correlation_id from the guard,
        then exit the context to release the slot before calling reply().

        :param spin_threshold: Spin count before futex sleep.
        :return: RpcRxGuard exposing the request buffer and correlation_id.
        """
        return self._rpc.serve(spin_threshold=spin_threshold)

    def reply(
            self,
            correlation_id: int,
            payload: bytes,
            msg_type: int,
            max_payload_size: Optional[int] = None,
    ) -> None:
        """
        Zero-copy write of a response into arena_rev.

        :param correlation_id: Must match the cid from the served request.
        :param payload: Response bytes.
        :param msg_type: Application-level message type.
        :param max_payload_size: Reserve size. Defaults to len(payload).
        """
        size = len(payload)
        reserve = max_payload_size if max_payload_size is not None else size

        with self._rpc.acquire_reply(
                correlation_id=correlation_id, max_payload_size=reserve
        ) as tx:
            with memoryview(tx) as m:
                m[:size] = payload
            tx.actual_size = size
            tx.msg_type = msg_type

    def reply_zero_copy(
            self, correlation_id: int, max_payload_size: int, msg_type: int
    ) -> _tachyon.RpcTxGuard:
        """
        Zero-copy TX reply guard. Caller fills the buffer, sets actual_size and
        msg_type, then exits the context.

        :param correlation_id: Must match the cid from the served request.
        :param max_payload_size: Maximum bytes to reserve in arena_rev.
        :param msg_type: Application-level message type.
        :return: RpcTxGuard context manager.
        """
        tx = self._rpc.acquire_reply(
            correlation_id=correlation_id, max_payload_size=max_payload_size
        )
        tx.msg_type = msg_type
        return tx
