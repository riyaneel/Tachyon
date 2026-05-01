from __future__ import annotations

import struct
from typing import Callable, Optional, TypeVar, TYPE_CHECKING

if TYPE_CHECKING:
	from .rpc import RpcBus

MSG_TYPE_ERROR: int = 0xFFFF
_ERROR_FMT = "!H"  # unhandled msg_type echoed back


def _encode_error(unhandled_mt: int) -> bytes:
	return struct.pack(_ERROR_FMT, unhandled_mt & 0xFFFF)


def _decode_error(payload: bytes) -> int:
	return struct.unpack(_ERROR_FMT, payload[:2])[0]


T = TypeVar("T")


class RpcEndpoint:
	"""
	Wraps a memoryview -> bytes handler with its msg_type contract.

	The handler receives a memoryview valid only for the duration of the call.
	Callable locally (no bus) for unit tests, payload is auto-wrapped in a memoryview,
	so the handler interface is identical in both contexts.
	"""

	def __init__(self, fn: Callable[[memoryview], bytes], msg_type: int) -> None:
		self._fn      = fn
		self.msg_type = msg_type
		self.__name__     = fn.__name__
		self.__qualname__ = fn.__qualname__
		self.__module__   = fn.__module__
		self.__doc__      = fn.__doc__
		self.__wrapped__  = fn

	def __call__(self, payload: bytes | memoryview) -> bytes:
		mv = memoryview(payload) if isinstance(payload, (bytes, bytearray)) else payload
		return self._fn(mv)

	def call(
			self,
			bus: "RpcBus",
			payload: bytes,
			on_response: Optional[Callable[[memoryview], T]] = None,
			spin_threshold: int = 10000,
	) -> "bytes | T":
		cid = bus.call(payload, msg_type=self.msg_type, spin_threshold=spin_threshold)
		with bus.wait(cid, spin_threshold=spin_threshold) as rx:
			with memoryview(rx) as mv:
				if on_response is not None:
					return on_response(mv)
				return mv.tobytes()

	def __repr__(self) -> str:
		return f"RpcEndpoint(msg_type={self.msg_type}, fn={self._fn.__name__!r})"


def tachyon_rpc(msg_type: int) -> Callable[[Callable[[memoryview], bytes]], RpcEndpoint]:
	"""
	Decorator: bind a memoryview→bytes function to an RPC message type.

	The handler receives a zero-copy memoryview of the request payload,
	valid only for the duration of the call. Call .tobytes() explicitly
	if you need a persistent copy.

	:param msg_type: Application-level message type (uint16, 0–65534).
					 65535 (0xFFFF) is reserved for protocol errors.
	"""
	if not (0 <= msg_type < MSG_TYPE_ERROR):
		raise ValueError(f"msg_type must be in [0, {MSG_TYPE_ERROR - 1}], got {msg_type}")

	def decorator(fn: Callable[[memoryview], bytes]) -> RpcEndpoint:
		return RpcEndpoint(fn, msg_type)

	return decorator


class RpcDispatcher:
	"""
	Maps incoming msg_type values to registered RpcEndpoint handlers.
	Runs the callee-side serve loop on an RpcBus.

	If no handler is registered for an incoming msg_type, a MSG_TYPE_ERROR reply is sent
	to the caller BEFORE raising locally.The caller is never left hanging.
	"""

	__slots__ = ("_handlers",)

	def __init__(self) -> None:
		self._handlers: dict[int, RpcEndpoint] = {}

	def register(self, endpoint: RpcEndpoint) -> "RpcDispatcher":
		"""
		Register an RpcEndpoint.

		:raise ValueError: Duplicate msg_type or reserved MSG_TYPE_ERROR.
		:return: self, for chaining.
		"""
		if endpoint.msg_type == MSG_TYPE_ERROR:
			raise ValueError(f"msg_type={MSG_TYPE_ERROR:#06x} is reserved for protocol errors")
		if endpoint.msg_type in self._handlers:
			existing = self._handlers[endpoint.msg_type].__name__
			raise ValueError(
				f"msg_type={endpoint.msg_type} already registered by {existing!r}"
			)
		self._handlers[endpoint.msg_type] = endpoint
		return self

	def handler(self, msg_type: int) -> Callable[[Callable[[memoryview], bytes]], RpcEndpoint]:
		"""
		Shorthand: decorate and register in one step.
		"""

		def decorator(fn: Callable[[memoryview], bytes]) -> RpcEndpoint:
			ep = RpcEndpoint(fn, msg_type)
			self.register(ep)
			return ep

		return decorator

	def serve_once(self, bus: "RpcBus", spin_threshold: int = 10000) -> None:
		"""
		Block until one request arrives, dispatch zero-copy to the handler, reply.

		If no handler is registered for the received msg_type, a MSG_TYPE_ERROR
		reply is sent to the caller so it is never left blocked, then KeyError
		is raised locally.

		:raise KeyboardInterrupt: Interrupted by signal during serve().
		:raise PeerDeadError:     FatalError on arena_fwd.
		:raise KeyError:          No handler for msg_type (after an error reply is sent).
		:raise Exception:         Any exception from the handler (after error reply sent).
		"""
		with bus.serve(spin_threshold=spin_threshold) as rx:
			cid = rx.correlation_id
			mt = rx.type_id
			handler = self._handlers.get(mt)
			with memoryview(rx) as mv:
				if handler is None:
					response = None
				else:
					try:
						response = handler._fn(mv)
					except Exception:
						response = None
						exc_to_raise = _encode_error(mt)
					# fall through: send error reply below
					else:
						exc_to_raise = None

		if handler is None:
			bus.reply(cid, _encode_error(mt), msg_type=MSG_TYPE_ERROR)
			raise KeyError(
				f"No handler registered for msg_type={mt} "
				f"(error reply sent to caller, cid={cid})"
			)

		if exc_to_raise is not None:
			bus.reply(cid, exc_to_raise, msg_type=MSG_TYPE_ERROR)
			raise RuntimeError(
				f"Handler {handler.__name__!r} raised an exception "
				f"(error reply sent to caller, cid={cid})"
			)

		bus.reply(cid, response, msg_type=mt)

	def serve_forever(self, bus: "RpcBus", spin_threshold: int = 10000) -> None:
		"""
		Blocking dispatch loop. Returns on KeyboardInterrupt or PeerDeadError.
		KeyError and RuntimeError from serve_once are logged and continued.

		:param bus:            Listening RpcBus (callee side).
		:param spin_threshold: Passed to every serve() call.
		"""
		while True:
			try:
				self.serve_once(bus, spin_threshold=spin_threshold)
			except (KeyError, RuntimeError):
				pass
