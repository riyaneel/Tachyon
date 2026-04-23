import os
import time
import struct
import threading
import pytest
import tachyon

SOCKET_PATH = "/tmp/tachyon_test.sock"
CAPACITY = 1 << 16


@pytest.fixture
def clean_socket():
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    yield SOCKET_PATH
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)


def test_bus_standard_api(clean_socket):
    payload = b"standard_payload_test"
    results = []

    def run_server():
        with tachyon.Bus.listen(clean_socket, CAPACITY) as server:
            msg = next(iter(server))
            results.append(msg)

    server_thread = threading.Thread(target=run_server)
    server_thread.start()

    time.sleep(0.1)

    with tachyon.Bus.connect(clean_socket) as client:
        client.send(payload, type_id=10)

    server_thread.join(timeout=2.0)

    assert len(results) == 1
    assert results[0].type_id == 10
    assert results[0].data == payload


def test_bus_zero_copy_api(clean_socket):
    payload = b"zero_copy_data"

    def run_server():
        with tachyon.Bus.listen(clean_socket, CAPACITY) as server:
            with server.recv_zero_copy() as rx:
                with memoryview(rx) as mv:
                    assert mv.tobytes() == payload
                assert rx.type_id == 42

    server_thread = threading.Thread(target=run_server)
    server_thread.start()

    time.sleep(0.1)

    with tachyon.Bus.connect(clean_socket) as client:
        with client.send_zero_copy(size=len(payload), type_id=42) as tx:
            with memoryview(tx) as mv:
                mv[:] = payload
            tx.actual_size = len(payload)

    server_thread.join(timeout=2.0)


def test_type_id_encoding():
    assert tachyon.make_type_id(0, 42) == 42
    assert tachyon.route_id(42) == 0
    assert tachyon.msg_type(42) == 42
    tid = tachyon.make_type_id(1, 99)
    assert tachyon.route_id(tid) == 1
    assert tachyon.msg_type(tid) == 99
    assert tachyon.make_type_id(0, 0) == 0
    assert tachyon.make_type_id(0xFFFF, 0xFFFF) == 0xFFFFFFFF
    with pytest.raises(ValueError):
        tachyon.make_type_id(-1, 0)
    with pytest.raises(ValueError):
        tachyon.make_type_id(0, 0x10000)


def test_type_id_equivalent_over_bus(clean_socket):
    results = []

    def run_server():
        with tachyon.Bus.listen(clean_socket, CAPACITY) as server:
            it = iter(server)
            results.append(next(it))
            results.append(next(it))

    server_thread = threading.Thread(target=run_server)
    server_thread.start()
    time.sleep(0.1)

    with tachyon.Bus.connect(clean_socket) as client:
        client.send(b"a", type_id=42)
        client.send(b"b", type_id=tachyon.make_type_id(0, 42))

    server_thread.join(timeout=2.0)

    assert results[0].type_id == results[1].type_id == 42
    assert tachyon.msg_type(results[0].type_id) == 42
    assert tachyon.route_id(results[0].type_id) == 0


def test_bus_dlpack_pytorch(clean_socket):
    try:
        import torch
    except ImportError:
        pytest.skip("PyTorch is not installed. Skipping DLPack zero-copy test.")

    data = struct.pack("4f", 1.5, 2.5, 3.5, 4.5)

    def run_server():
        with tachyon.Bus.listen(clean_socket, CAPACITY) as server:
            with server.drain_batch() as batch:
                msg = batch[0]
                tensor = torch.from_dlpack(msg).view(torch.float32)

                assert tensor.tolist() == [1.5, 2.5, 3.5, 4.5]
                assert msg.actual_size == 16
                assert msg.type_id == 99

                del tensor

    server_thread = threading.Thread(target=run_server)
    server_thread.start()

    time.sleep(0.1)

    with tachyon.Bus.connect(clean_socket) as client:
        with client.send_zero_copy(size=len(data), type_id=99) as tx:
            with memoryview(tx) as mv:
                mv[:] = data
            tx.actual_size = len(data)

    server_thread.join(timeout=2.0)
