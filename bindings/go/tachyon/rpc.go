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

type RpcBus struct {
	raw *C.tachyon_rpc_bus_t
}

type RpcRxGuard struct {
	bus           *RpcBus
	ptr           unsafe.Pointer
	size          int
	typeID        uint32
	correlationID uint64
	isServe       bool
	committed     bool
}

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

func (b *RpcBus) SetPollingMode(spinMode int) {
	if b.raw != nil {
		C.tachyon_rpc_set_polling_mode(b.raw, C.int(spinMode))
	}
}

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

func (g *RpcRxGuard) Data() []byte {
	return unsafe.Slice((*byte)(g.ptr), g.size)
}

func (g *RpcRxGuard) TypeID() uint32 {
	return g.typeID
}

func (g *RpcRxGuard) Size() int {
	return g.size
}

func (g *RpcRxGuard) CorrelationID() uint64 {
	return g.correlationID
}

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
