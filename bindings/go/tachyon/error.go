//go:build linux || darwin

package tachyon

/*
#include <tachyon.h>
*/
import "C"
import (
	"errors"
	"fmt"
)

type TachyonError struct {
	Code    int
	Message string
}

func (e *TachyonError) Error() string {
	return fmt.Sprintf("tachyon: %s (code: %d)", e.Message, e.Code)
}

// fromErrorT maps a tachyon_error_t to a Go error.
// Returns nil on TACHYON_SUCCESS.
func fromErrorT(code C.tachyon_error_t) error {
	switch code {
	case C.TACHYON_SUCCESS:
		return nil
	case C.TACHYON_ERR_NULL_PTR:
		return &TachyonError{Code: int(code), Message: "internal null pointer"}
	case C.TACHYON_ERR_MEM:
		return &TachyonError{Code: int(code), Message: "out of memory"}
	case C.TACHYON_ERR_OPEN:
		return &TachyonError{Code: int(code), Message: "failed to open shared memory fd"}
	case C.TACHYON_ERR_TRUNCATE:
		return &TachyonError{Code: int(code), Message: "failed to truncate shared memory"}
	case C.TACHYON_ERR_CHMOD:
		return &TachyonError{Code: int(code), Message: "failed to set shared memory permissions"}
	case C.TACHYON_ERR_SEAL:
		return &TachyonError{Code: int(code), Message: "failed to apply fcntl seals"}
	case C.TACHYON_ERR_MAP:
		return &TachyonError{Code: int(code), Message: "mmap() failed"}
	case C.TACHYON_ERR_INVALID_SZ:
		return &TachyonError{Code: int(code), Message: "capacity must be a strictly positive power of two"}
	case C.TACHYON_ERR_FULL:
		return &TachyonError{Code: int(code), Message: "ring buffer full (anti-overwrite shield)"}
	case C.TACHYON_ERR_EMPTY:
		return &TachyonError{Code: int(code), Message: "no messages available"}
	case C.TACHYON_ERR_NETWORK:
		return &TachyonError{Code: int(code), Message: "UNIX socket error"}
	case C.TACHYON_ERR_SYSTEM:
		return &TachyonError{Code: int(code), Message: "OS error"}
	case C.TACHYON_ERR_INTERRUPTED:
		return &TachyonError{Code: int(code), Message: "interrupted by signal"}
	case C.TACHYON_ERR_ABI_MISMATCH:
		return &TachyonError{
			Code: int(code),
			Message: "ABI mismatch: producer and consumer were compiled with incompatible " +
				"Tachyon versions or TACHYON_MSG_ALIGNMENT values: rebuild both sides from the same version",
		}
	default:
		return &TachyonError{Code: int(code), Message: fmt.Sprintf("unknown error code %d", int(code))}
	}
}

// IsABIMismatch reports whether err is a Tachyon ABI mismatch error.
func IsABIMismatch(err error) bool {
	var t *TachyonError
	ok := errors.As(err, &t)
	return ok && t.Code == int(C.TACHYON_ERR_ABI_MISMATCH)
}

// IsInterrupted reports whether err is a Tachyon interrupted error.
func IsInterrupted(err error) bool {
	var t *TachyonError
	ok := errors.As(err, &t)
	return ok && t.Code == int(C.TACHYON_ERR_INTERRUPTED)
}

// IsFull reports whether err is a ring buffer full error.
func IsFull(err error) bool {
	var t *TachyonError
	ok := errors.As(err, &t)
	return ok && t.Code == int(C.TACHYON_ERR_FULL)
}
