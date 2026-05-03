#!/usr/bin/env python3

import struct
import os

ARENA_CAPACITY = 4096
MSG_HDR_SIZE = 64  # TACHYON_MSG_ALIGNMENT = sizeof(MessageHeader)
TACHYON_MAGIC = 0x54414348
TACHYON_VERSION = 0x04
TACHYON_MSG_ALIGNMENT = 64
ARENA_HEADER_SIZE = 128  # alignas(128)
SPSC_INDICES_SIZE = 640  # 5 × alignas(128)
MEMORY_LAYOUT_SIZE = ARENA_HEADER_SIZE + SPSC_INDICES_SIZE  # 768
RPC_HDR_SIZE = 24  # sizeof(RpcPackedMeta)
SKIP_MARKER = 0xFFFFFFFF
UINT32_MAX = 0xFFFFFFFF


def header(size: int, type_id: int, reserved_size: int) -> bytes:
	return struct.pack("<III", size, type_id, reserved_size) + bytes(MSG_HDR_SIZE - 12)


def arena_rx_seed(head: int, hdr: bytes) -> bytes:
	return struct.pack("<Q", head) + hdr


def toctou_header(size: int, type_id: int, reserved_size: int) -> bytes:
	payload = struct.pack("<III", size, type_id, reserved_size)
	return payload + bytes(MSG_HDR_SIZE - len(payload))


def make_type_id(route: int, msg_type: int) -> int:
	return ((route & 0xFFFF) << 16) | (msg_type & 0xFFFF)


def make_tx_cmd(tail_mutation: int, tail_val: int, max_size: int,
                actual_size: int, type_id: int, action: int) -> bytes:
	return struct.pack("<BQIIIB",
	                   tail_mutation & 0xFF,
	                   tail_val & 0xFFFFFFFFFFFFFFFF,
	                   max_size & 0xFFFFFFFF,
	                   actual_size & 0xFFFFFFFF,
	                   type_id & 0xFFFFFFFF,
	                   action & 0xFF)


def make_rpc_cmd(max_size: int, actual_size: int, type_id: int,
                 correlation_id: int, force_case: int,
                 rx_payload: bytes = b"") -> bytes:
	cmd = struct.pack("<IIIQB",
	                  max_size & 0xFFFFFFFF,
	                  actual_size & 0xFFFFFFFF,
	                  type_id & 0xFFFFFFFF,
	                  correlation_id & 0xFFFFFFFFFFFFFFFF,
	                  force_case & 0xFF)
	return cmd + rx_payload


def make_shm_layout(magic: int, version: int, capacity: int,
                    msg_alignment: int, state: int = 2) -> bytes:
	hdr = struct.pack("<IIIII", magic, version, capacity, msg_alignment, state)
	hdr += bytes(ARENA_HEADER_SIZE - len(hdr))
	indices = bytes(SPSC_INDICES_SIZE)
	layout = hdr + indices
	assert len(layout) == MEMORY_LAYOUT_SIZE
	return layout


seeds_header_parser = {
	"valid_minimal": header(0, 0, MSG_HDR_SIZE),
	"skip_marker": header(SKIP_MARKER, 0, MSG_HDR_SIZE),
	"reserved_exact_min": header(0, 0, MSG_HDR_SIZE),
	"reserved_capacity": header(0, 0, ARENA_CAPACITY),
	"payload_max": header(ARENA_CAPACITY - MSG_HDR_SIZE, 0, ARENA_CAPACITY),
	"reserved_zero": header(0, 0, 0),
	"size_max_reserved64": header(UINT32_MAX, 0, MSG_HDR_SIZE),
	"reserved_unaligned": header(0, 0, MSG_HDR_SIZE + 1),
}

seeds_type_id = {
	"route0_msgtype0": header(0, make_type_id(0, 0), MSG_HDR_SIZE),
	"route0_msgtype1": header(0, make_type_id(0, 1), MSG_HDR_SIZE),
	"route0_msgtype_max": header(0, make_type_id(0, 0xFFFF), MSG_HDR_SIZE),
	"route1_msgtype0": header(0, make_type_id(1, 0), MSG_HDR_SIZE),
	"route1_msgtype1": header(0, make_type_id(1, 1), MSG_HDR_SIZE),
	"route1_msgtype_max": header(0, make_type_id(1, 0xFFFF), MSG_HDR_SIZE),
	"route_max_msgtype0": header(0, make_type_id(0xFFFF, 0), MSG_HDR_SIZE),
	"route_max_msgtype_max": header(0, make_type_id(0xFFFF, 0xFFFF), MSG_HDR_SIZE),
	"typeid_all_ones": header(0, UINT32_MAX, MSG_HDR_SIZE),
}

seeds_arena_rx = {
	"valid_minimal": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, MSG_HDR_SIZE)),
	"skip_marker": arena_rx_seed(MSG_HDR_SIZE, header(SKIP_MARKER, 0, MSG_HDR_SIZE)),
	"reserved_capacity": arena_rx_seed(ARENA_CAPACITY, header(0, 0, ARENA_CAPACITY)),
	"payload_max": arena_rx_seed(ARENA_CAPACITY, header(ARENA_CAPACITY - MSG_HDR_SIZE, 0, ARENA_CAPACITY)),
	"reserved_zero": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, 0)),
	"reserved_unaligned": arena_rx_seed(MSG_HDR_SIZE, header(0, 0, MSG_HDR_SIZE + 1)),
	"head_one": arena_rx_seed(1, header(0, 0, MSG_HDR_SIZE)),
	"head_batch_boundary": arena_rx_seed(32, header(0, 0, MSG_HDR_SIZE)),
	**{f"typeid_{k}": arena_rx_seed(MSG_HDR_SIZE, v) for k, v in seeds_type_id.items()},
}

seeds_arena_rx_batch = {
	"single_msg": arena_rx_seed(MSG_HDR_SIZE, header(0, 1, MSG_HDR_SIZE)),
	"batch_31": arena_rx_seed(MSG_HDR_SIZE * 31, header(0, 1, MSG_HDR_SIZE)) * 31,
	"batch_32": arena_rx_seed(MSG_HDR_SIZE * 32, header(0, 1, MSG_HDR_SIZE)) * 32,
	"batch_33": arena_rx_seed(MSG_HDR_SIZE * 33, header(0, 1, MSG_HDR_SIZE)) * 33,
	"batch_skip_last": (arena_rx_seed(MSG_HDR_SIZE, header(0, 1, MSG_HDR_SIZE)) * 31
	                    + arena_rx_seed(MSG_HDR_SIZE, header(SKIP_MARKER, 0, MSG_HDR_SIZE))),
	"batch_mixed_size": (arena_rx_seed(MSG_HDR_SIZE, header(0, 1, MSG_HDR_SIZE))
	                     + arena_rx_seed(MSG_HDR_SIZE * 2, header(8, 2, MSG_HDR_SIZE * 2))),
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

seeds_arena_tx = {
	"nominal_commit": make_tx_cmd(3, 0, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 0),
	"nominal_rollback": make_tx_cmd(3, 0, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 1),
	"tail_ahead_of_head": make_tx_cmd(0, 0, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 0),
	"tail_uint64_max": make_tx_cmd(1, 0, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 0),
	"tail_misaligned": make_tx_cmd(2, 64, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 0),
	"tail_arbitrary": make_tx_cmd(3, 0xDEADBEEF, MSG_HDR_SIZE, MSG_HDR_SIZE, 1, 0),
	"max_size_zero": make_tx_cmd(3, 0, 0, 0, 1, 0),
	"max_size_overflow": make_tx_cmd(3, 0, UINT32_MAX, 0, 1, 0),
	"actual_gt_max": make_tx_cmd(3, 0, MSG_HDR_SIZE, UINT32_MAX, 1, 0),
	"arena_full": make_tx_cmd(3, 0, ARENA_CAPACITY, ARENA_CAPACITY, 1, 0),
	"batch_boundary": make_tx_cmd(3, 0, MSG_HDR_SIZE, 0, 1, 0) * 33,
}

seeds_arena_rpc = {
	"nominal_valid_cid": make_rpc_cmd(RPC_HDR_SIZE, RPC_HDR_SIZE, 1, 42, 0),
	"cid_zero": make_rpc_cmd(RPC_HDR_SIZE, RPC_HDR_SIZE, 1, 0, 1),
	"cid_uint64_max": make_rpc_cmd(RPC_HDR_SIZE, RPC_HDR_SIZE, 1, 0, 2),
	"actual_gt_max": make_rpc_cmd(RPC_HDR_SIZE, UINT32_MAX, 1, 1, 0),
	"max_size_zero": make_rpc_cmd(0, 0, 1, 1, 0),
	"type_id_with_route": make_rpc_cmd(RPC_HDR_SIZE, RPC_HDR_SIZE, 0x00010001, 1, 0),
	"rx_cid_offset_corrupt": make_rpc_cmd(0, 0, 0, 0, 0, rx_payload=bytes([
		0x00, 0x00, 0x00, 0x40,  # actual_size=0, type_id low
		0x00, 0x00, 0x01, 0x00,  # type_id high, reserved_size low
		0x40, 0x00, 0xFF, 0xFF,  # reserved_size high, _pad non-zero
		0xDE, 0xAD, 0xBE, 0xEF,  # correlation_id bytes 0-3
		0xCA, 0xFE, 0xBA, 0xBE,  # correlation_id bytes 4-7
	])),
	"rx_pad_nonzero": make_rpc_cmd(0, 0, 0, 0, 0, rx_payload=bytes([
		0x04, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x18, 0x00, 0x00, 0x00,
		0xFF, 0xFF, 0x00, 0x00,  # _pad non-zero
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	])),
}

seeds_shm_attach = {
	"valid_4096": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 4096, TACHYON_MSG_ALIGNMENT) + bytes(4096),
	"bad_magic": make_shm_layout(0xDEADBEEF, TACHYON_VERSION, 4096, TACHYON_MSG_ALIGNMENT) + bytes(4096),
	"bad_version": make_shm_layout(TACHYON_MAGIC, 0x99, 4096, TACHYON_MSG_ALIGNMENT) + bytes(4096),
	"capacity_zero": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 0, TACHYON_MSG_ALIGNMENT) + bytes(4096),
	"capacity_not_pow2": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 4097, TACHYON_MSG_ALIGNMENT) + bytes(4097),
	"capacity_exceeds_span": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 0xFFFFFFFF, TACHYON_MSG_ALIGNMENT),
	"span_too_small": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 4096, TACHYON_MSG_ALIGNMENT),
	"bad_alignment": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 4096, 32) + bytes(4096),
	"capacity_one": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 1, TACHYON_MSG_ALIGNMENT) + bytes(1),
	"state_fatal": make_shm_layout(TACHYON_MAGIC, TACHYON_VERSION, 4096, TACHYON_MSG_ALIGNMENT, 4) + bytes(4096),
	"all_zeros": bytes(MEMORY_LAYOUT_SIZE + 4096),
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
	write_seeds("fuzz/corpus/header_parser", seeds_type_id)
	write_seeds("fuzz/corpus/arena_rx", seeds_arena_rx)
	write_seeds("fuzz/corpus/arena_rx_batch", seeds_arena_rx_batch)
	write_seeds("fuzz/corpus/toctou", seeds_toctou)
	write_seeds("fuzz/corpus/arena_tx", seeds_arena_tx)
	write_seeds("fuzz/corpus/arena_rpc", seeds_arena_rpc)
	write_seeds("fuzz/corpus/shm_attach", seeds_shm_attach)
	print("Done.")
