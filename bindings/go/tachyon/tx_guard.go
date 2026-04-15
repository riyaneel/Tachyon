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

// TxGuard holds a zero-copy write slot in the ring buffer.
//
// Write into the slot via Bytes(), then call Commit to publish or Rollback
// to cancel. The slot is invalid after either call.
//
// The finalizer calls Rollback if the guard is collected without being
// committed, this is a safety net, not the nominal path. Always call
// Commit or Rollback explicitly.
//
// Bytes() returns a slice directly into shared memory. Do not retain
// references to it after Commit or Rollback.
type TxGuard struct {
	bus       *Bus
	ptr       unsafe.Pointer
	maxSize   int
	committed bool
}

// AcquireTx acquires a TX slot of at least maxSize bytes.
// Blocks (spinning) until space is available.
//
// Must be followed by exactly one call to Commit or Rollback.
func (b *Bus) AcquireTx(maxSize int) (*TxGuard, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}

	var ptr unsafe.Pointer
	for {
		ptr = C.tachyon_acquire_tx(b.raw, C.size_t(maxSize))
		if ptr != nil {
			break
		}
		if C.tachyon_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return nil, &TachyonError{Code: int(C.TACHYON_ERR_SYSTEM), Message: "peer dead (fatal error state)"}
		}
	}

	g := &TxGuard{
		bus:     b,
		ptr:     ptr,
		maxSize: maxSize,
	}
	runtime.SetFinalizer(g, (*TxGuard).rollback)
	return g, nil
}

// Bytes return a writable slice of maxSize bytes directly into shared memory.
//
// The slice is valid only until Commit or Rollback is called.
// Do not retain references after either call.
func (g *TxGuard) Bytes() []byte {
	return unsafe.Slice((*byte)(g.ptr), g.maxSize)
}

// Commit publishes actualSize bytes with the given typeID and flushes.
// actualSize must be <= maxSize.
//
// Use CommitUnflushed when batch-sending multiple messages, call
// bus.Flush() after the last message.
func (g *TxGuard) Commit(actualSize int, typeID uint32) error {
	if g.committed {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "TxGuard already committed or rolled back"}
	}
	g.committed = true
	runtime.SetFinalizer(g, nil)

	err := C.tachyon_commit_tx(g.bus.raw, C.size_t(actualSize), C.uint32_t(typeID))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}
	C.tachyon_flush(g.bus.raw)
	return nil
}

// CommitUnflushed publishes actualSize bytes without flushing.
// The caller must call bus.Flush() after the last message in the batch
// to make all committed messages visible to the consumer.
func (g *TxGuard) CommitUnflushed(actualSize int, typeID uint32) error {
	if g.committed {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "TxGuard already committed or rolled back"}
	}
	g.committed = true
	runtime.SetFinalizer(g, nil)

	err := C.tachyon_commit_tx(g.bus.raw, C.size_t(actualSize), C.uint32_t(typeID))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}
	return nil
}

// Rollback cancels the TX transaction without publishing.
// Safe to call on an already committed guard (no-op).
func (g *TxGuard) Rollback() error {
	if g.committed {
		return nil
	}
	g.committed = true
	runtime.SetFinalizer(g, nil)

	err := C.tachyon_rollback_tx(g.bus.raw)
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}
	return nil
}

// rollback is the finalizer, called by the GC if the guard is never
// committed or rolled back explicitly.
func (g *TxGuard) rollback() {
	if !g.committed && g.bus != nil && g.bus.raw != nil {
		g.committed = true
		C.tachyon_rollback_tx(g.bus.raw)
	}
}
