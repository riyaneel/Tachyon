//go:build linux || darwin

package tachyon

/*
#cgo CFLAGS:   -I${SRCDIR}/_core_local/include
#cgo CXXFLAGS: -std=c++23 -O3 -march=native -fno-exceptions -fno-rtti
#cgo CXXFLAGS: -fvisibility=hidden -Wall -Wextra
#cgo CXXFLAGS: -I${SRCDIR}/_core_local/include

#cgo linux  LDFLAGS: -lrt -lstdc++
#cgo darwin LDFLAGS: -lc++

#include <tachyon.h>
*/
import "C"
