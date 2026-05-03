//go:build linux || darwin

package tachyon

/*
#include <stdlib.h>
#include <string.h>
#include <tachyon.h>
*/
import "C"
import (
	"runtime"
	"unsafe"
)

// RpcBus is a bidirectional RPC bus backed by two SPSC arenas.
//
// Use RpcListen on the callee side and RpcConnect on the caller side.
// Always call Close when done.
type RpcBus struct {
	raw *C.tachyon_rpc_bus_t
}

// RpcRxGuard holds a zero-copy read slot in either arena_fwd (Serve) or arena_rev (Wait).
//
// Read the payload via Data(), read the CorrelationID(), then call Commit to release the slot.
// The slot is invalid after Commit.
//
// Data() returns a slice directly into shared memory. Do not retain references after Commit.
type RpcRxGuard struct {
	bus           *RpcBus
	ptr           unsafe.Pointer
	size          int
	typeID        uint32
	correlationID uint64
	isServe       bool
	committed     bool
}

// RpcListen creates two SHM arenas and blocks until a caller connects via the UDS at socketPath
//
// capFwd is the arena capacity for incoming requests (caller -> callee).
// capRev is the arena capacity for incoming requests (callee -> caller).
// Both must be positive powers of two.
func RpcListen(socketPath string, capFwd, capRev int) (*RpcBus, error) {
	path := C.CString(socketPath)
	defer C.free(unsafe.Pointer(path))

	var raw *C.tachyon_rpc_bus_t
	for {
		err := C.tachyon_rpc_listen(path, C.size_t(capFwd), C.size_t(capRev), &raw)
		if err == C.TACHYON_ERR_INTERRUPTED {
			continue
		}

		if err != C.TACHYON_SUCCESS {
			return nil, fromErrorT(err)
		}

		break
	}

	b := &RpcBus{raw: raw}
	runtime.SetFinalizer(b, (*RpcBus).close)
	return b, nil
}

// RpcConnect attaches to an existing RPC bus via the UNIX socket at socketPath.
//
// Returns an error with IsABIMismatch(err) == true if the callee was compiled
// with a different Tachyon version, TACHYON_MSG_ALIGNMENT, or without the RPC
// flag.
func RpcConnect(socketPath string) (*RpcBus, error) {
	path := C.CString(socketPath)
	defer C.free(unsafe.Pointer(path))

	var raw *C.tachyon_rpc_bus_t
	err := C.tachyon_rpc_connect(path, &raw)
	if err != C.TACHYON_SUCCESS {
		return nil, fromErrorT(err)
	}

	b := &RpcBus{raw: raw}
	runtime.SetFinalizer(b, (*RpcBus).close)
	return b, nil
}

// Close unmaps both SHM arenas and releases all resources.
// Safe to call multiple times.
func (b *RpcBus) Close() error {
	b.close()
	runtime.SetFinalizer(b, nil)
	return nil
}

func (b *RpcBus) close() {
	if b.raw != nil {
		C.tachyon_rpc_destroy(b.raw)
		b.raw = nil
	}
}

// SetPollingMode signals that both arena consumers will never sleep.
// spinMode 1 enables pure-spin mode, 0 restores hybrid futex mode.
// Call immediately after RpcListen/RpcConnect, before the first message.
func (b *RpcBus) SetPollingMode(spinMode int) {
	if b.raw != nil {
		C.tachyon_rpc_set_polling_mode(b.raw, C.int(spinMode))
	}
}

// Call copies payload into arena_fwd, commits, and returns the assigned correlation_id. Blocks if the buffer is full.
//
// msgType is an application-defined message discriminator (uint16).
func (b *RpcBus) Call(payload []byte, msgType uint32) (uint64, error) {
	if b.raw == nil {
		return 0, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "RPC bus is closed"}
	}

	size := len(payload)
	var ptr unsafe.Pointer
	for {
		ptr = C.tachyon_rpc_acquire_tx(b.raw, C.size_t(size))
		if ptr != nil {
			break
		}

		if C.tachyon_rpc_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return 0, &TachyonError{
				Code:    int(C.TACHYON_ERR_SYSTEM),
				Message: "Peer dead (fatal error on arena_fwd)",
			}
		}
	}

	if size > 0 {
		C.memcpy(ptr, unsafe.Pointer(&payload[0]), C.size_t(size))
	}

	var outCid C.uint64_t
	err := C.tachyon_rpc_commit_call(b.raw, C.size_t(size), C.uint32_t(msgType), &outCid)
	if err != C.TACHYON_SUCCESS {
		return 0, fromErrorT(err)
	}

	return uint64(outCid), nil
}

// Wait blocks until the response matching correlationID arrives in arena_rev.
// Returns an RpcRxGuard. Must be followed by exactly one call to Commit.
//
// A correlation_id mismatch triggers FatalError on arena_rev and returns an
// error, the callee sent a reply out of order, which is a protocol violation.
//
// spinThreshold controls the spin-before-sleep threshold.
func (b *RpcBus) Wait(correlationId uint64, spinThreshold uint32) (*RpcRxGuard, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "RPC bus is closed"}
	}

	var actualSize C.size_t
	var msgType C.uint32_t
	var ptr unsafe.Pointer
	for {
		ptr = unsafe.Pointer(
			C.tachyon_rpc_wait(b.raw, C.uint64_t(correlationId), &actualSize, &msgType, C.uint32_t(spinThreshold)),
		)
		if ptr != nil {
			break
		}

		if C.tachyon_rpc_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return nil, &TachyonError{
				Code:    int(C.TACHYON_ERR_SYSTEM),
				Message: "correlation mismatch or peer dead (fatal error on arena_rev)",
			}
		}
	}

	g := &RpcRxGuard{
		bus:           b,
		ptr:           ptr,
		size:          int(actualSize),
		typeID:        uint32(msgType),
		correlationID: correlationId,
		isServe:       false,
	}
	runtime.SetFinalizer(g, (*RpcRxGuard).commit)

	return g, nil
}

// Serve blocks until a request arrives in arena_fwd.
// Returns an RpcRxGuard exposing the request payload and correlation_id.
// Must be followed by exactly one call to Commit, then Reply.
//
// spinThreshold controls the spin-before-sleep threshold.
func (b *RpcBus) Serve(spinThreshold uint32) (*RpcRxGuard, error) {
	if b.raw == nil {
		return nil, &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "rpc bus is closed"}
	}

	var correlationID C.uint64_t
	var msgType C.uint32_t
	var actualSize C.size_t

	var ptr unsafe.Pointer
	for {
		ptr = unsafe.Pointer(
			C.tachyon_rpc_serve(b.raw, &correlationID, &msgType, &actualSize, C.uint32_t(spinThreshold)),
		)
		if ptr != nil {
			break
		}

		if C.tachyon_rpc_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return nil, &TachyonError{
				Code:    int(C.TACHYON_ERR_SYSTEM),
				Message: "peer dead (fatal error on arena_fwd)",
			}
		}
	}

	g := &RpcRxGuard{
		bus:           b,
		ptr:           ptr,
		size:          int(actualSize),
		typeID:        uint32(msgType),
		correlationID: uint64(correlationID),
		isServe:       true,
	}
	runtime.SetFinalizer(g, (*RpcRxGuard).commit)

	return g, nil
}

// Reply copies payload into arena_rev as a response to correlationID.
// correlationID must match the value from the served RpcRxGuard.
// The RpcRxGuard must be Commit() before calling Reply.
//
// msgType is an application-defined response discriminator.
func (b *RpcBus) Reply(correlationID uint64, payload []byte, msgType uint32) error {
	if b.raw == nil {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "rpc bus is closed"}
	}

	size := len(payload)
	var ptr unsafe.Pointer
	for {
		ptr = C.tachyon_rpc_acquire_reply_tx(b.raw, C.size_t(size))
		if ptr != nil {
			break
		}

		if C.tachyon_rpc_get_state(b.raw) == C.TACHYON_STATE_FATAL_ERROR {
			return &TachyonError{
				Code:    int(C.TACHYON_ERR_SYSTEM),
				Message: "peer dead (fatal error on arena_rev)",
			}
		}
	}

	if size > 0 {
		C.memcpy(ptr, unsafe.Pointer(&payload[0]), C.size_t(size))
	}

	err := C.tachyon_rpc_commit_reply(b.raw, C.uint64_t(correlationID), C.size_t(size), C.uint32_t(msgType))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// Data returns a read-only slice directly into shared memory.
// Valid only until Commit is called.
func (g *RpcRxGuard) Data() []byte {
	return unsafe.Slice((*byte)(g.ptr), g.size)
}

// TypeID returns the message type discriminator set by the sender.
func (g *RpcRxGuard) TypeID() uint32 {
	return g.typeID
}

// Size returns the payload size in bytes.
func (g *RpcRxGuard) Size() int {
	return g.size
}

// CorrelationID returns the correlation_id of this message.
// On the callee side (Serve), use this value in Reply.
// On the caller side (Wait), this echoes the id passed to Wait.
func (g *RpcRxGuard) CorrelationID() uint64 {
	return g.correlationID
}

// Commit releases the ring buffer slot.
// Safe to call on an already committed guard (no-op).
func (g *RpcRxGuard) Commit() error {
	if g.committed {
		return nil
	}
	g.committed = true
	runtime.SetFinalizer(g, nil)

	var err C.tachyon_error_t
	if g.isServe {
		err = C.tachyon_rpc_commit_serve(g.bus.raw)
	} else {
		err = C.tachyon_rpc_commit_rx(g.bus.raw)
	}

	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}

	return nil
}

// commit is the finalizer, safety net only.
func (g *RpcRxGuard) commit() {
	if !g.committed && g.bus != nil && g.bus.raw != nil {
		g.committed = true
		if g.isServe {
			C.tachyon_rpc_commit_serve(g.bus.raw)
		} else {
			C.tachyon_rpc_commit_rx(g.bus.raw)
		}
	}
}
