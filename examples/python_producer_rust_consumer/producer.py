#!/usr/bin/env python3
import struct
import time
import tachyon

SOCKET_PATH = "/tmp/tachyon_market.sock"
CAPACITY = 1 << 20  # 1 MB
ITERATIONS = 1_000_000
BATCH_SIZE = 64  # flush every N messages

# MarketTick wire format (little-endian, 32 bytes)
TICK_FORMAT = "<8sdIQ4x"
TICK_SIZE = struct.calcsize(TICK_FORMAT)  # 32 bytes

TYPE_BID = 1
TYPE_ASK = 2
TYPE_SENTINEL = 0

# Pre-pack all tick variants — avoid struct.pack in the hot loop
_TICKS_RAW = [
    (struct.pack(TICK_FORMAT, b"AAPL\x00\x00\x00\x00", 189.42, 500, 0), TYPE_BID),
    (struct.pack(TICK_FORMAT, b"AAPL\x00\x00\x00\x00", 189.45, 300, 0), TYPE_ASK),
    (struct.pack(TICK_FORMAT, b"MSFT\x00\x00\x00\x00", 415.10, 1000, 0), TYPE_BID),
    (struct.pack(TICK_FORMAT, b"MSFT\x00\x00\x00\x00", 415.13, 750, 0), TYPE_ASK),
    (struct.pack(TICK_FORMAT, b"NVDA\x00\x00\x00\x00", 875.20, 200, 0), TYPE_BID),
    (struct.pack(TICK_FORMAT, b"NVDA\x00\x00\x00\x00", 875.25, 150, 0), TYPE_ASK),
]
N_VARIANTS = len(_TICKS_RAW)

SENTINEL_PAYLOAD = struct.pack(TICK_FORMAT, b"\x00" * 8, 0.0, 0, 0)


def main():
    print(f"[producer] Listening on {SOCKET_PATH} ...")
    print(f"[producer] Waiting for Rust consumer to connect ...")

    with tachyon.Bus.listen(SOCKET_PATH, CAPACITY) as bus:
        raw_bus = bus._bus
        print(f"[producer] Consumer connected. Sending {ITERATIONS:,} ticks "
              f"(batch={BATCH_SIZE}) ...")

        start = time.perf_counter_ns()

        for i in range(ITERATIONS):
            payload, type_id = _TICKS_RAW[i % N_VARIANTS]

            with raw_bus.acquire_tx(TICK_SIZE) as tx:
                with memoryview(tx) as mv:
                    mv[:TICK_SIZE] = payload
                    struct.pack_into("<Q", mv, 20, time.time_ns())
                tx.actual_size = TICK_SIZE
                tx.type_id = type_id

            if (i + 1) % BATCH_SIZE == 0:
                raw_bus.flush()

        # Sentinel — signals consumer to stop
        with raw_bus.acquire_tx(TICK_SIZE) as tx:
            with memoryview(tx) as mv:
                mv[:TICK_SIZE] = SENTINEL_PAYLOAD
            tx.actual_size = TICK_SIZE
            tx.type_id = TYPE_SENTINEL
        raw_bus.flush()

        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
        print(f"[producer] Done. {ITERATIONS:,} ticks in {elapsed_ms:.1f} ms "
              f"({ITERATIONS / elapsed_ms * 1000:.0f} ticks/sec)")


if __name__ == "__main__":
    main()
