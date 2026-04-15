//go:build linux || darwin

package tachyon

/*
#include <stdlib.h>
#include <tachyon.h>
*/
import "C"
import (
	"runtime"
	"unsafe"
)

// RxGuard holds a zero-copy read slot in the ring buffer.
//
// Read the payload via Data(), then call Commit to release the slot.
// The slot is invalid after Commit.
//
// The finalizer calls Commit if the guard is collected without being
// committed, safety net only, not the nominal path.
//
// Data() returns a slice directly into shared memory. Do not retain
// references to it after Commit.
type RxGuard struct {
	bus       *Bus
	ptr       unsafe.Pointer
	size      int
	typeID    uint32
	committed bool
}

// AcquireRx blocks until a message is available, then returns a zero-copy
// view into the ring buffer slot.
//
// spinThreshold controls the spin-before-sleep threshold.
// 10000 is a reasonable default for most workloads.
//
// Interrupted syscalls (EINTR from futex) are retried transparently.
//
// Must be followed by exactly one call to Commit.
func (b *Bus) AcquireRx(spinThreshold uint32) (*RxGuard, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
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
			return nil, &TachyonError{Code: int(C.TACHYON_ERR_SYSTEM), Message: "peer dead (fatal error state)"}
		}
	}

	g := &RxGuard{
		bus:    b,
		ptr:    ptr,
		size:   int(actualSize),
		typeID: uint32(typeID),
	}
	runtime.SetFinalizer(g, (*RxGuard).commit)

	return g, nil
}

// Data returns a read-only slice directly into shared memory.
//
// Valid only until Commit is called. Do not retain references after Commit.
func (g *RxGuard) Data() []byte {
	return unsafe.Slice((*byte)(g.ptr), g.size)
}

// TypeID returns the message type discriminator set by the producer.
func (g *RxGuard) TypeID() uint32 {
	return g.typeID
}

// Size returns the payload size in bytes.
func (g *RxGuard) Size() int {
	return g.size
}

// Commit releases the slot and advances the consumer tail.
// Safe to call on an already committed guard (no-op).
func (g *RxGuard) Commit() error {
	if g.committed {
		return nil
	}

	g.committed = true
	runtime.SetFinalizer(g, nil)

	err := C.tachyon_commit_rx(g.bus.raw)
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// commit is the finalizer.
func (g *RxGuard) commit() {
	if !g.committed && g.bus != nil && g.bus.raw != nil {
		g.committed = true
		C.tachyon_commit_rx(g.bus.raw)
	}
}
