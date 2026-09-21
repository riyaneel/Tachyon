// Page-specific demo program for the browser WASM example.
//
// This is the C++ equivalent of the previous Rust echo: it polls an inbound
// ring once, increments a little-endian u32 payload, and republishes it on an
// outbound ring. It deliberately drives the rings through the public Tachyon C
// ABI (`tachyon_acquire_rx`, `tachyon_acquire_tx`, ...) so the demo runs on the
// exact same fuzzed/sanitized core as the JavaScript side. Compiled into the
// example's single WASM module alongside the core via Emscripten.

#include <cstdint>
#include <cstring>

#include <emscripten.h>
#include <tachyon.h>

extern "C" {

/// Echoes one message between two page-local rings.
///
/// Reads a 4-byte u32 from `inbound`, increments it, and writes the result to
/// `outbound` with the route bumped by one. Returns 1 when a message was
/// processed, 0 when `inbound` was empty, and -1 on a malformed payload.
EMSCRIPTEN_KEEPALIVE
int tachyon_browser_echo_once(tachyon_bus_t *inbound, tachyon_bus_t *outbound) {
	uint32_t	type_id	    = 0;
	size_t		actual_size = 0;
	const void *in_ptr	    = tachyon_acquire_rx(inbound, &type_id, &actual_size);
	if (in_ptr == nullptr) {
		return 0;
	}

	if (actual_size != sizeof(uint32_t)) {
		tachyon_commit_rx(inbound);
		return -1;
	}

	uint32_t value = 0;
	std::memcpy(&value, in_ptr, sizeof(value));
	value += 1;

	void *out_ptr = tachyon_acquire_tx(outbound, sizeof(value));
	if (out_ptr == nullptr) {
		tachyon_commit_rx(inbound);
		return -1;
	}

	std::memcpy(out_ptr, &value, sizeof(value));
	tachyon_commit_tx(outbound, sizeof(value), type_id + (1U << 16));
	tachyon_flush(outbound);
	tachyon_commit_rx(inbound);
	return 1;
}

} // extern "C"
