//go:build linux || darwin

package tachyon

/*
#include <stdlib.h>
#include <string.h>
#include <tachyon.h>
*/
import "C"
import "unsafe"

// Send copies of data into the ring buffer, commits, and flushes.
// Blocks if the buffer is full until space is available.
// typeID is an application-defined message discriminator.
func (b *Bus) Send(data []byte, typeID uint32) error {
	if b.raw == nil {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}

	size := len(data)

	var ptr unsafe.Pointer
	for {
		ptr = C.tachyon_acquire_tx(b.raw, C.size_t(size))
		if ptr != nil {
			break
		}
		if C.tachyon_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return &TachyonError{Code: int(C.TACHYON_ERR_SYSTEM), Message: "peer dead (fatal error state)"}
		}
	}

	if size > 0 {
		C.memcpy(ptr, unsafe.Pointer(&data[0]), C.size_t(size))
	}

	err := C.tachyon_commit_tx(b.raw, C.size_t(size), C.uint32_t(typeID))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	C.tachyon_flush(b.raw)
	return nil
}

// Recv blocks until a message is available, copies the payload into a new
// []byte, commits the slot, and returns (data, typeID, error).
//
// spinThreshold controls the spin-before-sleep threshold. 10000 is a reasonable default for most workloads.
//
// The caller owns the returned slice, safe to use after Recv returns.
func (b *Bus) Recv(spinThreshold uint32) ([]byte, uint32, error) {
	if b.raw == nil {
		return nil, 0, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}

	var typeID C.uint32_t
	var actualSize C.size_t

	var ptr unsafe.Pointer
	for {
		ptr = unsafe.Pointer(
			C.tachyon_acquire_rx_blocking(b.raw, &typeID, &actualSize, C.uint32_t(spinThreshold)),
		)
		if ptr != nil {
			break
		}
		if C.tachyon_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return nil, 0, &TachyonError{Code: int(C.TACHYON_ERR_SYSTEM), Message: "peer dead (fatal error state)"}
		}
	}

	out := make([]byte, int(actualSize))
	if int(actualSize) > 0 {
		C.memcpy(unsafe.Pointer(&out[0]), ptr, actualSize)
	}

	err := C.tachyon_commit_rx(b.raw)
	if err != C.TACHYON_SUCCESS {
		return nil, 0, fromErrorT(err)
	}

	return out, uint32(typeID), nil
}
