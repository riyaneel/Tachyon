#!/usr/bin/env python3
import time
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

if not HAS_TORCH and not HAS_NUMPY:
    raise SystemExit(
        "This example requires torch or numpy.\n"
        "  pipenv install torch   or   pipenv install numpy"
    )

SOCKET_PATH = "/tmp/tachyon_cpp_inference.sock"
CAPACITY = 1 << 23  # 8 MB

TYPE_FEATURES = 1
TYPE_SENTINEL = 0

FRAME_FLOATS = 256
FRAME_SIZE = FRAME_FLOATS * 4  # 1024 bytes
BATCH_SIZE = 256  # frames per compute batch
REPORT_EVERY = 50_000

if HAS_TORCH:
    WEIGHTS = torch.randn(FRAME_FLOATS, 64, dtype=torch.float32)
else:
    WEIGHTS = np.random.randn(FRAME_FLOATS, 64).astype(np.float32)


def main():
    backend = "torch" if HAS_TORCH else "numpy"
    print(f"[consumer] Backend     : {backend}")
    print(f"[consumer] Batch       : {BATCH_SIZE} frames/compute")
    print(f"[consumer] Inference   : [{BATCH_SIZE},{FRAME_FLOATS}] × "
          f"[{FRAME_FLOATS},64] matmul")
    print(f"[consumer] Listening on {SOCKET_PATH} ...")
    print(f"[consumer] Waiting for C++ producer to connect ...\n")

    with tachyon.Bus.listen(SOCKET_PATH, CAPACITY) as bus:
        raw_bus = bus._bus
        bus.set_polling_mode(1)
        print("[consumer] Producer connected. Receiving feature vectors ...\n")

        # Pre-allocated accumulation buffer — no per-frame allocation.
        accum = np.zeros((BATCH_SIZE, FRAME_FLOATS), dtype=np.float32)
        batch_idx = 0
        total = 0
        result_sum = 0.0
        done = False
        start = time.perf_counter_ns()

        while not done:
            # consumer_lock held only for the duration of the with block:
            # one header read + one np.frombuffer row copy (~100 ns).
            with raw_bus.acquire_rx() as rx:
                if rx.type_id == TYPE_SENTINEL:
                    done = True
                    break

                with memoryview(rx) as mv:
                    accum[batch_idx] = np.frombuffer(mv, dtype=np.float32)

            # consumer_lock released — C++ producer unblocked immediately.
            batch_idx += 1

            if batch_idx == BATCH_SIZE:
                # Batch compute entirely outside SHM context.
                if HAS_TORCH:
                    with torch.no_grad():
                        t = torch.from_numpy(accum)
                        out = t @ WEIGHTS
                else:
                    out = accum @ WEIGHTS

                result_sum += float(out.sum())
                total += BATCH_SIZE
                batch_idx = 0

                if total % REPORT_EVERY < BATCH_SIZE:
                    elapsed_ms = (time.perf_counter_ns() - start) / 1e6
                    fps = total / elapsed_ms * 1000
                    print(
                        f"[consumer] {total:>8} frames | "
                        f"result_sum={result_sum:.1f} | "
                        f"{fps:.0f} frames/sec"
                    )

        # Flush remaining partial batch.
        if batch_idx > 0:
            if HAS_TORCH:
                with torch.no_grad():
                    out = torch.from_numpy(accum[:batch_idx]) @ WEIGHTS
            else:
                out = accum[:batch_idx] @ WEIGHTS
            result_sum += float(out.sum())
            total += batch_idx

        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
        print(f"\n[consumer] Summary")
        print(f"  Total frames  : {total:,}")
        print(f"  Backend       : {backend}")
        print(f"  Batch size    : {BATCH_SIZE} (compute)")
        print(f"  Elapsed       : {elapsed_ms:.1f} ms")
        print(f"  Throughput    : {total / elapsed_ms * 1000:.0f} frames/sec")
        print(f"  Result sum    : {result_sum:.2f}")
        print(
            f"  Bandwidth     : "
            f"{total * FRAME_SIZE / 1e9 / (elapsed_ms / 1000):.2f} GB/s"
        )


if __name__ == "__main__":
    main()
