import os
import threading
import time
import struct

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
