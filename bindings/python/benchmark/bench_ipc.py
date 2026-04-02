import os
import time
import sys
import argparse
import multiprocessing as mp
import tachyon

sys.stdout.reconfigure(line_buffering=True)

CAPACITY = 1 << 24  # 16MB
MSG_SIZE = 4096  # 4KB
ITERATIONS = 100_000
SOCKET_PING = "/tmp/tachyon_ping.sock"
SOCKET_PONG = "/tmp/tachyon_pong.sock"
SOCKET_THROUGHPUT = "/tmp/tachyon_throughput.sock"


def wait_for_bus_connect(path: str, retries: int = 100) -> tachyon.Bus:
    for _ in range(retries):
        try:
            return tachyon.Bus.connect(path)
        except (ConnectionError, FileNotFoundError, tachyon.TachyonError):
            time.sleep(0.01)
    raise RuntimeError(f"Failed to connect to {path} after {retries} attempts.")


def run_latency_server():
    print("[Server] Listening on PING bus...")
    with tachyon.Bus.listen(SOCKET_PING, CAPACITY) as rx_bus:
        rx_bus.set_polling_mode(1)
        print("[Server] Client connected to PING. Now listening on PONG bus...")
        with tachyon.Bus.listen(SOCKET_PONG, CAPACITY) as tx_bus:
            print("[Server] Both buses initialized. Starting loop.")
            it = iter(rx_bus)
            for _ in range(ITERATIONS):
                msg = next(it)
                tx_bus.send(msg.data, type_id=msg.type_id)


def run_latency_client():
    data = b"x" * MSG_SIZE
    print("[Client] Connecting to PING bus...")
    tx_bus = wait_for_bus_connect(SOCKET_PING)

    with tx_bus:
        print("[Client] Connecting to PONG bus...")
        rx_bus = wait_for_bus_connect(SOCKET_PONG)
        rx_bus.set_polling_mode(1)

        with rx_bus:
            print("[Client] Starting Latency Benchmark...")
            it = iter(rx_bus)
            start = time.perf_counter_ns()
            for _ in range(ITERATIONS):
                tx_bus.send(data)
                next(it)
            end = time.perf_counter_ns()

            duration_ms = (end - start) / 1e6
            avg_lat_us = ((end - start) / ITERATIONS) / 1000 / 2
            print(f"\n--- Latency Results ---")
            print(f"Iterations: {ITERATIONS} | MsgSize: {MSG_SIZE} bytes")
            print(f"Total time: {duration_ms:.2f} ms")
            print(f"Avg One-Way Latency: {avg_lat_us:.3f} us")


def run_throughput_receiver():
    print("[Receiver] Listening on THROUGHPUT bus...")
    with tachyon.Bus.listen(SOCKET_THROUGHPUT, CAPACITY) as bus:
        bus.set_polling_mode(1)
        it = iter(bus)
        for _ in range(ITERATIONS):
            next(it)


def run_throughput_sender():
    data = b"x" * MSG_SIZE
    bus = wait_for_bus_connect(SOCKET_THROUGHPUT)
    with bus:
        print("[Sender] Starting Throughput Benchmark...")
        start = time.perf_counter_ns()
        for _ in range(ITERATIONS):
            while True:
                try:
                    bus.send(data)
                    break
                except tachyon.TachyonError:
                    time.sleep(0.000001)
        end = time.perf_counter_ns()

    duration_s = (end - start) / 1e9
    total_mb = (MSG_SIZE * ITERATIONS) / (1024 * 1024)
    print(f"\n--- Throughput Results ---")
    print(f"Sent: {total_mb:.2f} MB in {duration_s:.4f} s")
    print(f"Bandwidth: {total_mb / duration_s:.2f} MB/s")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Tachyon IPC Benchmark")
    parser.add_argument("--mode", choices=["latency", "throughput"], required=True)
    args = parser.parse_args()

    for path in [SOCKET_PING, SOCKET_PONG, SOCKET_THROUGHPUT]:
        if os.path.exists(path):
            os.unlink(path)

    try:
        if args.mode == "latency":
            p = mp.Process(target=run_latency_server)
            p.start()
            run_latency_client()
            p.join(timeout=1.0)
        else:
            p = mp.Process(target=run_throughput_receiver)
            p.start()
            run_throughput_sender()
            p.join(timeout=1.0)
    finally:
        if p.is_alive():
            p.terminate()
