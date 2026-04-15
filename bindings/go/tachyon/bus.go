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

// Bus is a Tachyon SPSC IPC bus.
//
// Bus is safe to pass between goroutines but must be used by at most one
// goroutine at a time per direction (one producer, one consumer).
//
// Always call Close when done, or rely on the finalizer as a safety net.
// The finalizer is not a substitute for explicit Close, it fires
// non-deterministically and may delay SHM release.
type Bus struct {
	raw *C.tachyon_bus_t
}

// Listen creates a new SHM arena and waits for one consumer to connect via
// the UNIX socket at socketPath.
//
// This call blocks until a consumer calls Connect on the same path.
// Interrupted syscalls (EINTR) are retried transparently.
//
// capacity must be a strictly positive power of two.
func Listen(socketPath string, capacity int) (*Bus, error) {
	path := C.CString(socketPath)
	defer C.free(unsafe.Pointer(path))

	var raw *C.tachyon_bus_t

	for {
		err := C.tachyon_bus_listen(path, C.size_t(capacity), &raw)
		if err == C.TACHYON_ERR_INTERRUPTED {
			continue
		}
		if err != C.TACHYON_SUCCESS {
			return nil, fromErrorT(err)
		}
		break
	}

	b := &Bus{raw: raw}
	runtime.SetFinalizer(b, (*Bus).close)
	return b, nil
}

// Connect attaches to an existing SHM arena via the UNIX socket at socketPath.
//
// Returns an error wrapping TachyonError with IsABIMismatch(err) == true if
// the producer was compiled with a different Tachyon version or
// TACHYON_MSG_ALIGNMENT value.
func Connect(socketPath string) (*Bus, error) {
	path := C.CString(socketPath)
	defer C.free(unsafe.Pointer(path))

	var raw *C.tachyon_bus_t
	err := C.tachyon_bus_connect(path, &raw)
	if err != C.TACHYON_SUCCESS {
		return nil, fromErrorT(err)
	}

	b := &Bus{raw: raw}
	runtime.SetFinalizer(b, (*Bus).close)
	return b, nil
}

// Close unmaps the shared memory and releases all resources.
// Safe to call multiple times.
func (b *Bus) Close() error {
	b.close()
	runtime.SetFinalizer(b, nil)
	return nil
}

// close is the internal destructor used by both Close and the finalizer.
func (b *Bus) close() {
	if b.raw != nil {
		C.tachyon_bus_destroy(b.raw)
		b.raw = nil
	}
}

// Flush publishes all pending TX messages to the consumer.
// Must be called after one or more AcquireTx/Commit sequences to make
// messages visible when batch-sending (commit_unflushed pattern).
func (b *Bus) Flush() {
	if b.raw != nil {
		C.tachyon_flush(b.raw)
	}
}

// SetNumaNode binds the shared memory backing this bus to a specific NUMA
// node using MPOL_PREFERRED + MPOL_MF_MOVE.
//
// Call immediately after Listen or Connect, before the first message, to
// ensure all ring buffer pages are on the desired node before the hot path.
//
// No-op on non-Linux platforms.
//
// nodeID must be in [0, 63]. Returns an error if mbind() fails (invalid node
// or missing CAP_SYS_NICE).
func (b *Bus) SetNumaNode(nodeID int) error {
	if b.raw == nil {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}
	err := C.tachyon_bus_set_numa_node(b.raw, C.int(nodeID))
	if err != C.TACHYON_SUCCESS {
		return fromErrorT(err)
	}
	return nil
}

// SetPollingMode signals that the consumer will never sleep, skipping the
// futex wake check on every producer flush.
//
// When spinMode is 1, the producer omits the atomic_thread_fence(seq_cst)
// and the consumer_sleeping load on every Flush call. Use only when the
// consumer goroutine is dedicated and will never park, if it parks, the
// producer will not issue a futex wake and the consumer will spin
// indefinitely instead of sleeping.
//
// Call immediately after Listen or Connect, before the first message.
// spinMode 1 enables pure-spin mode, 0 restores hybrid mode.
func (b *Bus) SetPollingMode(spinMode int) error {
	if b.raw == nil {
		return &TachyonError{Code: int(C.TACHYON_ERR_NULL_PTR), Message: "bus is closed"}
	}

	C.tachyon_bus_set_polling_mode(b.raw, C.int(spinMode))
	return nil
}
