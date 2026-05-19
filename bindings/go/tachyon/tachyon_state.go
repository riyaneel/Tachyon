//go:build linux || darwin

package tachyon

/*
#include <stdlib.h>
#include <tachyon.h>
*/
import "C"

type TachyonState int

const (
	StateUninitialized TachyonState = iota
	StateInitializing
	StateReady
	StateDisconnected
	StateFatalError
	StateUnknown
)

func (s TachyonState) String() string {
	switch s {
	case StateUninitialized:
		return "Uninitialized"
	case StateInitializing:
		return "Initializing"
	case StateReady:
		return "Ready"
	case StateDisconnected:
		return "Disconnected"
	case StateFatalError:
		return "FatalError"
	default:
		return "Unknown"
	}
}

func stateFromC(c C.tachyon_state_t) TachyonState {
	return TachyonState(c)
}
