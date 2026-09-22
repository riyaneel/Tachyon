import os
import threading
import time
import struct
from array import array

import pytest
import tachyon

SOCKET_PATH = "/tmp/tachyon_rpc_test.sock"
CAP = 1 << 16


@pytest.fixture
def clean_socket():
	if os.path.exists(SOCKET_PATH):
		os.unlink(SOCKET_PATH)
	yield SOCKET_PATH
	if os.path.exists(SOCKET_PATH):
		os.unlink(SOCKET_PATH)


def test_roundtrip(clean_socket):
	results = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			with callee.serve() as rx:
				cid = rx.correlation_id
				with memoryview(rx) as mv:
					req = mv.tobytes()
			callee.reply(cid, req[::-1], msg_type=2)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		payload = b"hello"
		cid = caller.call(payload, msg_type=1)
		with caller.wait(cid) as rx:
			with memoryview(rx) as mv:
				results.append((rx.type_id, mv.tobytes()))

	t.join(timeout=2.0)

	assert results[0] == (2, b"olleh")


def test_zero_copy_roundtrip(clean_socket):
	results = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			with callee.serve() as rx:
				cid = rx.correlation_id
				mt = rx.type_id
				with memoryview(rx) as mv:
					req_bytes = mv.tobytes()
			with callee.reply_zero_copy(cid, len(req_bytes), msg_type=mt + 1) as tx:
				with memoryview(tx) as mv:
					mv[: len(req_bytes)] = req_bytes
				tx.actual_size = len(req_bytes)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		payload = b"zerocopy"
		with caller.call_zero_copy(len(payload), msg_type=7) as tx:
			with memoryview(tx) as mv:
				mv[: len(payload)] = payload
			tx.actual_size = len(payload)
		cid = tx.out_cid

		with caller.wait(cid) as rx:
			with memoryview(rx) as mv:
				results.append((rx.type_id, mv.tobytes()))

	t.join(timeout=2.0)

	assert results[0] == (8, b"zerocopy")


def test_correlation_id_monotonic(clean_socket):
	cids = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			for _ in range(4):
				with callee.serve() as rx:
					cid = rx.correlation_id
				callee.reply(cid, b"ok", msg_type=0)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		for i in range(4):
			cid = caller.call(b"x", msg_type=0)
			cids.append(cid)
			with caller.wait(cid):
				pass

	t.join(timeout=2.0)

	assert cids == list(range(cids[0], cids[0] + 4))
	assert all(c > 0 for c in cids)


def test_n_inflight_ordered(clean_socket):
	N = 8
	sent = [i * 100 for i in range(N)]
	received = {}

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP * 4, CAP * 4) as callee:
			callee.set_polling_mode(1)
			for _ in range(N):
				with callee.serve() as rx:
					cid = rx.correlation_id
					with memoryview(rx) as mv:
						val = struct.unpack("I", mv[:4])[0]
				callee.reply(cid, struct.pack("I", val), msg_type=0)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		caller.set_polling_mode(1)
		cids = []
		for v in sent:
			cid = caller.call(struct.pack("I", v), msg_type=0)
			cids.append(cid)

		for i, cid in enumerate(cids):
			with caller.wait(cid) as rx:
				with memoryview(rx) as mv:
					received[cid] = struct.unpack("I", mv[:4])[0]

	t.join(timeout=2.0)

	for i, cid in enumerate(cids):
		assert received[cid] == sent[i]


def test_serve_type_id_preserved(clean_socket):
	results = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			with callee.serve() as rx:
				results.append(rx.type_id)
				cid = rx.correlation_id
			callee.reply(cid, b".", msg_type=0)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		cid = caller.call(b".", msg_type=42)
		with caller.wait(cid):
			pass

	t.join(timeout=2.0)

	assert results[0] == 42


def test_context_manager_cleanup(clean_socket):
	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			with callee.serve() as rx:
				cid = rx.correlation_id
			callee.reply(cid, b"ok", msg_type=0)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	bus = tachyon.RpcBus.rpc_connect(clean_socket)
	cid = bus.call(b"cleanup", msg_type=0)
	with bus.wait(cid):
		pass
	bus.__exit__(None, None, None)

	t.join(timeout=2.0)


def test_struct_payload(clean_socket):
	fmt = "2d"
	sent = (3.14, 2.71)
	results = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			with callee.serve() as rx:
				cid = rx.correlation_id
				with memoryview(rx) as mv:
					a, b = struct.unpack(fmt, mv[: struct.calcsize(fmt)])
			callee.reply(cid, struct.pack(fmt, a * 2, b * 2), msg_type=1)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		payload = struct.pack(fmt, *sent)
		cid = caller.call(payload, msg_type=0)
		with caller.wait(cid) as rx:
			with memoryview(rx) as mv:
				results.append(struct.unpack(fmt, mv[: struct.calcsize(fmt)]))

	t.join(timeout=2.0)

	assert results[0] == pytest.approx((sent[0] * 2, sent[1] * 2))


def test_dispatcher_and_endpoint(clean_socket):
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()

	@dispatcher.handler(msg_type=99)
	def reverse_handler(mv: memoryview) -> bytes:
		return mv.tobytes()[::-1]

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			dispatcher.serve_once(callee)

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		payload = b"tachyon"
		resp = reverse_handler.call(caller, payload)

	t.join(timeout=2.0)

	assert resp == b"noyhcat"


def test_dispatcher_unhandled_msg_type(clean_socket):
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			try:
				dispatcher.serve_once(callee)
			except KeyError:
				pass

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		cid = caller.call(b"unhandled", msg_type=404)
		with caller.wait(cid) as rx:
			assert rx.type_id == 0xFFFF
			with memoryview(rx) as mv:
				unhandled_mt = struct.unpack("!H", mv[:2])[0]
				assert unhandled_mt == 404

	t.join(timeout=2.0)


def test_dispatcher_handler_exception(clean_socket):
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()

	@dispatcher.handler(msg_type=50)
	def crash_handler(mv: memoryview) -> bytes:
		raise ValueError("Intentional crash")

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			try:
				dispatcher.serve_once(callee)
			except RuntimeError:
				pass

	t = threading.Thread(target=run_callee)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		cid = caller.call(b"crash", msg_type=50)
		with caller.wait(cid) as rx:
			assert rx.type_id == 0xFFFF
			with memoryview(rx) as mv:
				crashed_mt = struct.unpack("!H", mv[:2])[0]
				assert crashed_mt == 50

	t.join(timeout=2.0)


@pytest.mark.parametrize(
	"bad_return",
	[
		pytest.param(lambda mv: None, id="none"),
		pytest.param(lambda mv: "not bytes", id="str"),
		pytest.param(lambda mv: 42, id="int"),
		pytest.param(lambda mv: memoryview(array("I", [1, 2, 3])), id="array"),
		pytest.param(lambda mv: mv, id="echo_mv"),
		pytest.param(lambda mv: mv[:4], id="echo_slice"),
	],
)
def test_dispatcher_handler_bad_return_value(clean_socket, bad_return):
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()
	dispatcher.handler(msg_type=60)(bad_return)

	callee_errors = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			try:
				dispatcher.serve_once(callee)
			except Exception as exc:
				callee_errors.append(exc)

	t = threading.Thread(target=run_callee, daemon=True)
	t.start()
	time.sleep(0.05)

	received = []

	def run_caller():
		with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
			cid = caller.call(b"bad", msg_type=60)
			with caller.wait(cid) as rx:
				with memoryview(rx) as mv:
					received.append((rx.type_id, struct.unpack("!H", mv[:2])[0]))

	c = threading.Thread(target=run_caller, daemon=True)
	c.start()
	c.join(timeout=2.0)
	t.join(timeout=2.0)

	assert not c.is_alive(), "caller is still blocked: no error reply was sent"
	assert received == [(0xFFFF, 60)]
	assert len(callee_errors) == 1
	assert isinstance(callee_errors[0], RuntimeError)
	assert callee_errors[0].__cause__ is not None


def test_dispatcher_serve_forever_survives_bad_handler(clean_socket):
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()

	@dispatcher.handler(msg_type=70)
	def forgot_to_return(mv: memoryview) -> bytes:
		mv.tobytes()

	@dispatcher.handler(msg_type=71)
	def echo(mv: memoryview) -> bytes:
		return mv.tobytes()

	@dispatcher.handler(msg_type=72)
	def stop(mv: memoryview) -> bytes:
		raise KeyboardInterrupt

	@dispatcher.handler(msg_type=73)
	def echo_slice(mv: memoryview) -> bytes:
		# Keeps a view of the request alive past the call: the serve() guard
		# commits the slot and raises BufferError on exit.
		return mv[:4]

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			try:
				dispatcher.serve_forever(callee)
			except KeyboardInterrupt:
				pass

	t = threading.Thread(target=run_callee, daemon=True)
	t.start()
	time.sleep(0.05)

	received = []

	def run_caller():
		with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
			cid = caller.call(b"bad", msg_type=70)
			with caller.wait(cid) as rx:
				received.append(rx.type_id)
			cid = caller.call(b"slice", msg_type=73)
			with caller.wait(cid) as rx:
				received.append(rx.type_id)
			received.append(echo.call(caller, b"still alive"))
			caller.call(b"stop", msg_type=72)

	c = threading.Thread(target=run_caller, daemon=True)
	c.start()
	c.join(timeout=2.0)
	t.join(timeout=2.0)

	assert not c.is_alive(), "caller is still blocked after the bad handler"
	assert received == [0xFFFF, 0xFFFF, b"still alive"]
	assert not t.is_alive(), "serve_forever did not return on KeyboardInterrupt"


@pytest.mark.parametrize(
	"msg_type, reply_error, expected",
	[
		pytest.param(80, tachyon.TachyonError("arena_rev full"), RuntimeError, id="handler_ring_full"),
		pytest.param(81, tachyon.TachyonError("arena_rev full"), KeyError, id="no_handler_ring_full"),
		pytest.param(80, tachyon.PeerDeadError("peer gone"), tachyon.PeerDeadError, id="peer_dead"),
	],
)
def test_dispatcher_error_reply_is_best_effort(clean_socket, monkeypatch, msg_type, reply_error, expected):
	"""A failing error reply must not replace the failure being reported; only a
	dead peer propagates (so serve_forever keeps going on a full arena_rev)."""
	from tachyon import RpcDispatcher

	dispatcher = RpcDispatcher()

	@dispatcher.handler(msg_type=80)
	def crash(mv: memoryview) -> bytes:
		raise ValueError("handler failed")

	def reply_fails(self, *args, **kwargs):
		raise reply_error

	monkeypatch.setattr(tachyon.RpcBus, "reply", reply_fails)

	callee_errors = []

	def run_callee():
		with tachyon.RpcBus.rpc_listen(clean_socket, CAP, CAP) as callee:
			try:
				dispatcher.serve_once(callee)
			except Exception as exc:
				callee_errors.append(exc)

	t = threading.Thread(target=run_callee, daemon=True)
	t.start()
	time.sleep(0.05)

	with tachyon.RpcBus.rpc_connect(clean_socket) as caller:
		caller.call(b"x", msg_type=msg_type)
		t.join(timeout=2.0)

	assert len(callee_errors) == 1
	err = callee_errors[0]
	assert isinstance(err, expected)
	if expected is not tachyon.PeerDeadError:
		assert "could not be sent" in str(err)
	if expected is RuntimeError:
		assert isinstance(err.__cause__, ValueError)
