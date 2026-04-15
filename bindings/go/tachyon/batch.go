//go:build linux || darwin

package tachyon

/*
#include <stdlib.h>
#include <tachyon.h>
*/
import "C"
import (
	"iter"
	"runtime"
	"unsafe"
)

// RxMsg is a zero-copy view into one message inside an RxBatch.
//
// Data() is valid only until RxBatch.Commit is called.
// Do not retain references to Data() after Commit.
type RxMsg struct {
	ptr    unsafe.Pointer
	size   int
	typeID uint32
}

// Data returns a read-only slice directly into shared memory.
// Invalid after the parent RxBatch is committed.
func (m *RxMsg) Data() []byte {
	return unsafe.Slice((*byte)(m.ptr), m.size)
}

// TypeID returns the message type discriminator set by the producer.
func (m *RxMsg) TypeID() uint32 {
	return m.typeID
}

// Size returns the payload size in bytes.
func (m *RxMsg) Size() int {
	return m.size
}

// RxBatch holds a batch of zero-copy read slots drained from the ring buffer
// in a single CGO crossing.
//
// Iterate via At(i), Len(), or the range-over-func Iter().
// Call Commit when done, this releases all slots atomically.
//
// All RxMsg.Data() slices are invalid after Commit.
// The finalizer calls Commit if the batch is collected without being committed.
type RxBatch struct {
	bus       *Bus
	views     []C.tachyon_msg_view_t
	msgs      []RxMsg
	count     int
	committed bool
}

// newRxBatch constructs an RxBatch from a filled views slice of length n.
func newRxBatch(bus *Bus, views []C.tachyon_msg_view_t, n int) *RxBatch {
	msgs := make([]RxMsg, n)
	for i := range n {
		msgs[i] = RxMsg{
			ptr:    unsafe.Pointer(views[i].ptr),
			size:   int(views[i].actual_size),
			typeID: uint32(views[i].type_id),
		}
	}
	batch := &RxBatch{
		bus:   bus,
		views: views[:n],
		msgs:  msgs,
		count: n,
	}
	runtime.SetFinalizer(batch, (*RxBatch).commit)

	return batch
}

// TryDrainBatch is the non-blocking variant of DrainBatch.
// Returns nil, nil immediately if no messages are available.
// Never sleeps, suitable for polling loops.
//
// If messages are available, returns an RxBatch that must be committed.
func (b *Bus) TryDrainBatch(maxMsgs int) (*RxBatch, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}
	if maxMsgs <= 0 {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_INVALID_SZ), Message: "maxMsgs must be > 0"}
	}

	views := make([]C.tachyon_msg_view_t, maxMsgs)
	count := C.tachyon_acquire_rx_batch(b.raw, &views[0], C.size_t(maxMsgs))
	if count == 0 {
		return nil, nil
	}

	return newRxBatch(b, views, int(count)), nil
}

// DrainBatch blocks until at least one message is available, then drains up
// to maxMsgs messages from the ring buffer in a single CGO crossing.
//
// spinThreshold controls the spin-before-sleep threshold.
// 10000 is a reasonable default.
//
// Interrupted syscalls (EINTR from futex) are retried transparently.
//
// Must be followed by exactly one call to Commit.
func (b *Bus) DrainBatch(maxMsgs int, spinThreshold uint32) (*RxBatch, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}
	if maxMsgs <= 0 {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_INVALID_SZ), Message: "maxMsgs must be > 0"}
	}

	views := make([]C.tachyon_msg_view_t, maxMsgs)

	var count C.size_t
	for {
		count = C.tachyon_drain_batch(
			b.raw,
			&views[0],
			C.size_t(maxMsgs),
			C.uint32_t(spinThreshold),
		)
		if count > 0 {
			break
		}
		if C.tachyon_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return nil, &TachyonError{Code: int(C.TACHYON_ERR_SYSTEM), Message: "peer dead (fatal error state)"}
		}
	}

	return newRxBatch(b, views, int(count)), nil
}

// Len returns the number of messages in the batch.
func (b *RxBatch) Len() int {
	return b.count
}

// At returns the message at index i.
// Panics if i is out of range.
func (b *RxBatch) At(i int) *RxMsg {
	if i < 0 || i >= b.count {
		panic("tachyon: RxBatch index out of range")
	}

	return &b.msgs[i]
}

// Iter returns a range-over-func iterator over all messages in the batch.
//
//	for msg := range batch.Iter() {
//	    process(msg.Data())
//	}
//
// Do not call Commit inside the loop, all Data() slices become invalid
// immediately after Commit.
func (b *RxBatch) Iter() iter.Seq[*RxMsg] {
	return func(yield func(*RxMsg) bool) {
		for i := range b.count {
			if !yield(&b.msgs[i]) {
				return
			}
		}
	}
}

// Commit releases all slots in the batch and advances the consumer tail.
// All RxMsg.Data() slices are invalid after this call.
// Safe to call on an already committed batch (no-op).
func (b *RxBatch) Commit() error {
	if b.committed {
		return nil
	}
	b.committed = true
	runtime.SetFinalizer(b, nil)

	err := C.tachyon_commit_rx_batch(b.bus.raw, &b.views[0], C.size_t(b.count))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// commit is the finalizer.
func (b *RxBatch) commit() {
	if !b.committed && b.bus != nil && b.bus.raw != nil && b.count > 0 {
		b.committed = true
		C.tachyon_commit_rx_batch(b.bus.raw, &b.views[0], C.size_t(b.count))
	}
}
