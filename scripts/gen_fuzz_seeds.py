#!/usr/bin/env python3

import struct
import os

ARENA_CAPACITY = 4096
MSG_HDR_SIZE = 64  # TACHYON_MSG_ALIGNMENT = sizeof(MessageHeader)


def header(size: int, type_id: int, reserved_size: int) -> bytes:
    return struct.pack("<III", size, type_id, reserved_size) + bytes(MSG_HDR_SIZE - 12)


def arena_rx_seed(head: int, hdr: bytes) -> bytes:
    return struct.pack("<Q", head) + hdr


def toctou_header(size: int, type_id: int, reserved_size: int) -> bytes:
    payload = struct.pack("<III", size, type_id, reserved_size)
    return payload + bytes(MSG_HDR_SIZE - len(payload))


SKIP_MARKER = 0xFFFFFFFF
UINT32_MAX = 0xFFFFFFFF

seeds_header_parser = {
    "valid_minimal": header(0, 0, MSG_HDR_SIZE),
    "skip_marker": header(SKIP_MARKER, 0, MSG_HDR_SIZE),
    "reserved_exact_min": header(0, 0, MSG_HDR_SIZE),  # reserved == sizeof(Header)
    "reserved_capacity": header(0, 0, ARENA_CAPACITY),
    "payload_max": header(ARENA_CAPACITY - MSG_HDR_SIZE, 0, ARENA_CAPACITY),
    "reserved_zero": header(0, 0, 0),  # underflow path
    "size_max_reserved64": header(UINT32_MAX, 0, MSG_HDR_SIZE),  # size > reserved - hdr
    "reserved_unaligned": header(0, 0, MSG_HDR_SIZE + 1),  # not aligned to 64
}

seeds_arena = {
    "valid_minimal": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, MSG_HDR_SIZE)),
    "skip_marker": arena_rx_seed(MSG_HDR_SIZE, header(SKIP_MARKER, 0, MSG_HDR_SIZE)),
    "reserved_capacity": arena_rx_seed(ARENA_CAPACITY, header(0, 0, ARENA_CAPACITY)),
    "payload_max": arena_rx_seed(ARENA_CAPACITY, header(ARENA_CAPACITY - MSG_HDR_SIZE, 0, ARENA_CAPACITY)),
    "reserved_zero": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, 0)),
    "reserved_unaligned": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, MSG_HDR_SIZE + 1)),
    "head_one": arena_rx_seed(1, header(0, 0, MSG_HDR_SIZE)),
    "head_batch_boundary": arena_rx_seed(32, header(0, 0, MSG_HDR_SIZE)),  # BATCH_SIZE
}

seeds_toctou = {
    "underflow_reserved_zero": toctou_header(0, 0, 0),
    "underflow_reserved_one": toctou_header(0, 0, 1),
    "underflow_reserved_63": toctou_header(0, 0, 63),
    "underflow_size_max": toctou_header(UINT32_MAX, 0, 0),
    "underflow_size_max_reserved63": toctou_header(UINT32_MAX, 0, 63),
    "misaligned_65": toctou_header(0, 0, 65),
    "misaligned_127": toctou_header(0, 0, 127),
    "misaligned_129": toctou_header(0, 0, 129),
    "valid_reserved_64_size_0": toctou_header(0, 0, 64),
    "valid_reserved_128_size_64": toctou_header(64, 0, 128),
    "reserved_at_capacity": toctou_header(0, 0, ARENA_CAPACITY),
    "reserved_exceeds_capacity": toctou_header(0, 0, ARENA_CAPACITY + 1),
    "skip_in_reserved": toctou_header(0, 0, UINT32_MAX),
}


def write_seeds(directory: str, seeds: dict[str, bytes]) -> None:
    os.makedirs(directory, exist_ok=True)
    for name, data in seeds.items():
        path = os.path.join(directory, f"seed_{name}")
        with open(path, "wb") as f:
            f.write(data)
        print(f"  wrote {path} ({len(data)} bytes)")


if __name__ == "__main__":
    print("Generating fuzz seeds...")
    write_seeds("fuzz/corpus/header_parser", seeds_header_parser)
    write_seeds("fuzz/corpus/arena_rx", seeds_arena)
    write_seeds("fuzz/corpus/arena_rx_batch", seeds_arena)
    write_seeds("fuzz/corpus/toctou", seeds_toctou)
    print("Done.")
