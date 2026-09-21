#!/usr/bin/env python3
import sys
from pathlib import Path

EXPECTED = {
	"tachyon_bus_listen",
	"tachyon_bus_destroy",
	"tachyon_bus_get_shm_ptr",
	"tachyon_bus_set_polling_mode",
	"tachyon_acquire_tx",
	"tachyon_commit_tx",
	"tachyon_acquire_rx_batch",
	"tachyon_commit_rx_batch",
	"tachyon_rollback_tx",
	"tachyon_acquire_rx",
	"tachyon_commit_rx",
	"tachyon_flush",
	"tachyon_get_state",
	"emmalloc_malloc",
	"emmalloc_free",
	"__wasm_call_ctors",
	"_emscripten_stack_alloc",
	"_emscripten_stack_restore",
	"emscripten_stack_get_current",
}

KIND_FUNC = 0
KIND_MEMORY = 2


def _u32(buf, i):
	result = shift = 0
	while True:
		byte = buf[i]
		i += 1
		result |= (byte & 0x7F) << shift
		if not byte & 0x80:
			return result, i

		shift += 7


def _name(buf, i):
	length, i = _u32(buf, i)
	return buf[i:i + length].decode("utf-8", "replace"), i + length


def sections(buf):
	if buf[:4] != b"\x00asm":
		raise ValueError("not a WebAssembly module")

	i = 8
	while i < len(buf):
		section_id = buf[i]
		i += 1
		size, i = _u32(buf, i)
		yield section_id, buf[i:i + size]
		i += size


def parse_exports(payload):
	funcs = {}
	has_memory = False
	count, i = _u32(payload, 0)

	for _ in range(count):
		name, i = _name(payload, i)
		kind = payload[i]
		i += 1
		index, i = _u32(payload, i)
		if kind == KIND_FUNC:
			funcs[name] = index
		elif kind == KIND_MEMORY:
			has_memory = True

	return funcs, has_memory


def parse_function_names(payload):
	section_name, i = _name(payload, 0)
	if section_name != "name":
		return None

	names = {}
	while i < len(payload):
		subsection_id = payload[i]
		i += 1
		size, i = _u32(payload, i)

		if subsection_id == 1:
			body = payload[i:i + size]
			count, j = _u32(body, 0)
			for _ in range(count):
				index, j = _u32(body, j)
				names[index], j = _name(body, j)

		i += size

	return names


def main(argv):
	if len(argv) != 2:
		sys.exit(__doc__)

	path = Path(argv[1])
	buf = path.read_bytes()

	exports = None
	has_memory = False
	func_names = None
	for section_id, payload in sections(buf):
		if section_id == 7:
			exports, has_memory = parse_exports(payload)
		elif section_id == 0:
			parsed = parse_function_names(payload)
			if parsed is not None:
				func_names = parsed

	if exports is None:
		sys.exit(f"{path}: no export section")
	if func_names is None:
		sys.exit(f"{path}: no `name` section.")

	resolved = {}
	unnamed = []
	for export_name, index in exports.items():
		symbol = func_names.get(index)
		if symbol is None:
			unnamed.append(f"{export_name} (func {index})")
		else:
			resolved.setdefault(symbol, []).append(export_name)

	found = set(resolved)
	unexpected = sorted(found - EXPECTED)
	missing = sorted(EXPECTED - found)
	folded = sorted(
		(symbol, names) for symbol, names in resolved.items() if len(names) > 1
	)

	print(f"[exports] {path.name}: {len(exports)} function exports")
	print(f"[exports] {len(found & EXPECTED)}/{len(EXPECTED)} declared symbols present")

	for symbol, names in folded:
		print(f"[Warning]: {symbol} is exported under {len(names)} names ({', '.join(sorted(names))})")

	if not has_memory:
		print("[Error]: the module does not export its memory; HEAPU8 will be unavailable")

	for entry in sorted(unnamed):
		print(f"[Error]: exported function has no name section entry: {entry}")

	for symbol in unexpected:
		print(f"[Error]: {symbol} is exported but is not part of the declared surface")

	for symbol in missing:
		print(f"[Error]: {symbol} is declared in the surface but is not exported")

	if unexpected or missing or unnamed or not has_memory:
		sys.exit("The module's export surface does not match the declared one.")

	print("[exports] surface matches the declared ABI.")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
