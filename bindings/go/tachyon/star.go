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

// StarBus aggregates N independent SPSC arenas polled in round-robin.
// Each spoke is a Bus created by the producer side (Listen).
// The star holds connector-side (Connect) handles, one per spoke.
//
// Always call Close when done.
type StarBus struct {
	raw *C.tachyon_star_t
}

func Create(buses []*Bus, nodeIds []int) (*StarBus, error) {
	n := len(buses)
	if n == 0 {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_INVALID_SZ), Message: "buses must not be empty"}
	}

	if nodeIds != nil && len(nodeIds) != n {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_INVALID_SZ), Message: "nodeIds length must equal len(buses)"}
	}

	rawBuses := make([]*C.tachyon_bus_t, n)
	for i, b := range buses {
		if b == nil || b.raw == nil {
			return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is nil or closed"}
		}
		rawBuses[i] = b.raw
	}

	var nodeIDsPtr *C.int
	if nodeIds != nil {
		cids := make([]C.int, n)
		for i, id := range nodeIds {
			cids[i] = C.int(id)
		}
		nodeIDsPtr = &cids[0]
	}

	var out *C.tachyon_star_t
	if err := C.tachyon_star_create(&rawBuses[0], C.size_t(n), nodeIDsPtr, &out); err != C.TACHYON_SUCCESS {
		return nil, fromErrorT(err)
	}

	star := &StarBus{raw: out}
	runtime.SetFinalizer(star, (*StarBus).close)
	return star, nil
}

// Close destroys the StarBus and releases all internal bus references.
func (s *StarBus) Close() error {
	s.close()
	runtime.SetFinalizer(s, nil)
	return nil
}

func (s *StarBus) close() {
	if s.raw != nil {
		C.tachyon_star_destroy(s.raw)
		s.raw = nil
	}
}

// NSpokes return the number of spokes.
func (s *StarBus) NSpokes() int {
	if s.raw == nil {
		return 0
	}

	return int(C.tachyon_star_n_spokes(s.raw))
}

// GetState returns the arena state of spoke spokeIdx.
// Returns StateUnknown if spokeIdx is out of range or the bus is closed.
func (s *StarBus) GetState(spokeIdx int) TachyonState {
	if s.raw == nil {
		return StateUnknown
	}

	return stateFromC(C.tachyon_star_get_state(s.raw, C.size_t(spokeIdx)))
}

// Poll drains up to maxTotal messages across all spokes within budgetUs microseconds.
// Returns an empty guard if no messages arrived before the budget expired.
//
// Must be followed by exactly one Commit on the returned guard. Do not call Poll again until the previous guard is committed.
func (s *StarBus) Poll(maxTotal int, budgetUs uint64) (*StarPollGuard, error) {
	if s.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "star bus is closed"}
	}

	if maxTotal <= 0 {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_INVALID_SZ), Message: "maxTotal must be > 0"}
	}

	views := make([]C.tachyon_msg_view_t, maxTotal)
	spokeIdxs := make([]C.size_t, maxTotal)
	n := int(C.tachyon_star_poll(
		s.raw,
		&views[0],
		C.size_t(maxTotal),
		C.uint64_t(budgetUs),
		&spokeIdxs[0],
	))

	msgs := make([]StarMsg, n)
	for i := range n {
		msgs[i] = StarMsg{
			ptr:      unsafe.Pointer(views[i].ptr),
			size:     int(views[i].actual_size),
			typeID:   uint32(views[i].type_id),
			spokeIdx: int(spokeIdxs[i]),
		}
	}

	g := &StarPollGuard{star: s, msgs: msgs}
	runtime.SetFinalizer(g, (*StarPollGuard).commit)
	return g, nil
}

// AcquireTx acquires a TX slot of maxSize bytes on spoke spokeIdx.
// Returns nil if the ring is full or spokeIdx is out of range.
// Must be followed by exactly one Commit or Rollback.
func (s *StarBus) AcquireTx(spokeIdx, maxSize int) *StarTxGuard {
	if s.raw == nil {
		return nil
	}

	ptr := C.tachyon_star_acquire_tx(s.raw, C.size_t(spokeIdx), C.size_t(maxSize))
	if ptr == nil {
		return nil
	}

	g := &StarTxGuard{
		star:     s,
		ptr:      ptr,
		maxSize:  maxSize,
		spokeIdx: spokeIdx,
	}

	runtime.SetFinalizer(g, (*StarTxGuard).rollback)
	return g
}

// Flush publishes pending unflushed TX data on spoke spokeIdx.
// Not needed after StarTxGuard.Commit, which flushes internally.
func (s *StarBus) Flush(spokeIdx int) {
	if s.raw != nil {
		C.tachyon_star_flush(s.raw, C.size_t(spokeIdx))
	}
}

// StarMsg is a zero-copy view into one message inside a StarPollGuard.
// Data() is valid only until the parent guard is committed.
type StarMsg struct {
	ptr      unsafe.Pointer
	size     int
	typeID   uint32
	spokeIdx int
}

// Data returns a read-only slice directly into shared memory.
// Invalid after the parent StarPollGuard is committed.
func (m *StarMsg) Data() []byte {
	if m.ptr == nil || m.size == 0 {
		return nil
	}

	return unsafe.Slice((*byte)(m.ptr), m.size)
}

// TypeID returns the raw type_id. Use RouteID / MsgType to decode.
func (m *StarMsg) TypeID() uint32 {
	return m.typeID
}

// SpokeIdx returns the spoke index this message was received on.
func (m *StarMsg) SpokeIdx() int {
	return m.spokeIdx
}

// Size returns the payload size in bytes.
func (m *StarMsg) Size() int {
	return m.size
}

// StarPollGuard holds a zero-copy batch drained from a StarBus.Poll call.
// All StarMsg.Data() slices point directly into shared memory and are invalid after Commit.
// The finalizer commits if the guard is collected without an explicit call.
type StarPollGuard struct {
	star      *StarBus
	msgs      []StarMsg
	committed bool
}

// Len returns the number of messages in the guard.
func (g *StarPollGuard) Len() int {
	return len(g.msgs)
}

// IsEmpty reports whether the guard contains no messages.
func (g *StarPollGuard) IsEmpty() bool {
	return len(g.msgs) == 0
}

// At returns the message at index i. Panics if it is out of range.
func (g *StarPollGuard) At(i int) *StarMsg {
	if i < 0 || i >= len(g.msgs) {
		panic("tachyon: StarPollGuard index out of range")
	}

	return &g.msgs[i]
}

// Iter returns a range-over-func iterator over all messages in the guard.
func (g *StarPollGuard) Iter() iter.Seq[*StarMsg] {
	return func(yield func(*StarMsg) bool) {
		for i := range g.msgs {
			if !yield(&g.msgs[i]) {
				return
			}
		}
	}
}

// Commit advances the ring-buffer tail for all polled spokes and releases the slots.
// All StarMsg.Data() slices become invalid. Safe to call multiple times (no-op after the first).
func (g *StarPollGuard) Commit() error {
	if g.committed {
		return nil
	}

	g.committed = true
	runtime.SetFinalizer(g, nil)
	if err := C.tachyon_star_commit(g.star.raw); err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// commit is the finalizer. Safety net only.
func (g *StarPollGuard) commit() {
	if !g.committed && g.star != nil && g.star.raw != nil {
		g.committed = true
		C.tachyon_star_commit(g.star.raw)
	}
}

// StarTxGuard holds a zero-copy TX slot on a specific spoke.
// Write the payload via Bytes(), then call Commit or Rollback.
// Drop without Commit → automatic Rollback via finalizer.
type StarTxGuard struct {
	star      *StarBus
	ptr       unsafe.Pointer
	maxSize   int
	spokeIdx  int
	committed bool
}

// Bytes return a writable slice of maxSize bytes into shared memory.
// Invalid after Commit or Rollback.
func (g *StarTxGuard) Bytes() []byte {
	return unsafe.Slice((*byte)(g.ptr), g.maxSize)
}

// Commit publishes actualSize bytes with typeID and flushes the spoke.
// actualSize must be ≤ maxSize.
func (g *StarTxGuard) Commit(actualSize int, typeID uint32) error {
	if g.committed {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "StarTxGuard already committed or rolled back"}
	}

	g.committed = true
	runtime.SetFinalizer(g, nil)
	if err := C.tachyon_star_commit_tx(
		g.star.raw, C.size_t(g.spokeIdx), C.size_t(actualSize), C.uint32_t(typeID),
	); err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// Rollback cancels the TX slot without publishing. No-op if already committed.
func (g *StarTxGuard) Rollback() error {
	if g.committed {
		return nil
	}

	g.committed = true
	runtime.SetFinalizer(g, nil)
	if err := C.tachyon_star_rollback_tx(g.star.raw, C.size_t(g.spokeIdx)); err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// rollback is the finalizer. Safety net only.
func (g *StarTxGuard) rollback() {
	if !g.committed && g.star != nil && g.star.raw != nil {
		g.committed = true
		C.tachyon_star_rollback_tx(g.star.raw, C.size_t(g.spokeIdx))
	}
}
