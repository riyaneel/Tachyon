import os
import time
import threading
import pytest
import tachyon

SOCKET_PATH = "/tmp/tachyon_test.sock"
CAPACITY = 1 << 16 # 64KB


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
