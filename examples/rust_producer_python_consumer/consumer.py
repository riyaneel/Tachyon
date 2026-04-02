#!/usr/bin/env python3
import time
import struct
import tachyon

try:
    import numpy as np

    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

try:
    import torch

    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

SOCKET_PATH = "/tmp/tachyon_inference.sock"
CAPACITY = 1 << 22  # 4 MB

TYPE_FEATURES = 1
TYPE_SENTINEL = 0

FRAME_SIZE = 256 * 4  # 256 × f32 = 1024 bytes
BATCH_SIZE = 256  # frames accumulated before compute
REPORT_EVERY = 50_000


def main():
    backend = "torch" if HAS_TORCH else ("numpy" if HAS_NUMPY else "pure python")
    print(f"[consumer] Backend  : {backend}")
    print(f"[consumer] Batch    : {BATCH_SIZE} frames/compute")
    print(f"[consumer] Listening on {SOCKET_PATH} ...")
    print(f"[consumer] Waiting for Rust producer to connect ...")

    with tachyon.Bus.listen(SOCKET_PATH, CAPACITY) as bus:
        raw_bus = bus._bus
        bus.set_polling_mode(1)
        print(f"[consumer] Producer connected. Receiving feature vectors ...\n")

        total = 0
        mean_sum = 0.0
        done = False
        start = time.perf_counter_ns()

        # Pre-allocated accumulation buffer — avoids per-frame allocation.
        if HAS_NUMPY:
            accum = np.zeros((BATCH_SIZE, 256), dtype=np.float32)

        batch_idx = 0

        while not done:
            # acquire_rx: consumer_lock held only for the duration of
            # the with block (header read + memcpy to accum buffer).
            with raw_bus.acquire_rx() as rx:
                if rx.type_id == TYPE_SENTINEL:
                    done = True
                    break

                if HAS_NUMPY:
                    with memoryview(rx) as mv:
                        # Zero-copy view into SHM, copied into pre-allocated
                        # row before consumer_lock is released on with-exit.
                        accum[batch_idx] = np.frombuffer(mv, dtype=np.float32)
                else:
                    with memoryview(rx) as mv:
                        raw_bytes = bytes(mv)

            # consumer_lock released here — producer unblocked immediately.

            if HAS_NUMPY:
                batch_idx += 1
                if batch_idx == BATCH_SIZE:
                    # Batch compute entirely outside SHM context.
                    if HAS_TORCH:
                        t = torch.from_numpy(accum)
                        means = t.mean(dim=1)
                        mean_sum += float(means.sum())
                    else:
                        means = accum.mean(axis=1)
                        mean_sum += float(means.sum())

                    total += BATCH_SIZE
                    batch_idx = 0

                    if total % REPORT_EVERY < BATCH_SIZE:
                        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
                        fps = total / elapsed_ms * 1000
                        print(
                            f"[consumer] {total:>8} frames | "
                            f"last mean={float(means[-1]):.2f} | "
                            f"{fps:.0f} frames/sec"
                        )
            else:
                values = struct.unpack_from(f"<{256}f", raw_bytes)
                mean = sum(values) / len(values)
                mean_sum += mean
                total += 1

        # Flush remaining partial batch
        if HAS_NUMPY and batch_idx > 0:
            if HAS_TORCH:
                means = torch.from_numpy(accum[:batch_idx]).mean(dim=1)
                mean_sum += float(means.sum())
            else:
                means = accum[:batch_idx].mean(axis=1)
                mean_sum += float(means.sum())
            total += batch_idx

        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
        print(f"\n[consumer] Summary")
        print(f"  Total frames : {total:,}")
        print(f"  Backend      : {backend}")
        print(f"  Batch size   : {BATCH_SIZE} (compute)")
        print(f"  Elapsed      : {elapsed_ms:.1f} ms")
        print(f"  Throughput   : {total / elapsed_ms * 1000:.0f} frames/sec")
        print(f"  Avg mean     : {mean_sum / total:.6f}")
        print(
            f"  Bandwidth    : "
            f"{total * FRAME_SIZE / 1e9 / (elapsed_ms / 1000):.2f} GB/s"
        )


if __name__ == "__main__":
    main()
