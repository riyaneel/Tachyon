set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if (NOT DEFINED LLVM_VERSION)
	set(LLVM_VERSION 21)
endif ()

find_program(CMAKE_C_COMPILER NAMES clang-${LLVM_VERSION} clang)
find_program(CMAKE_CXX_COMPILER NAMES clang++-${LLVM_VERSION} clang++)
if (NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
	message(FATAL_ERROR "[toolchain/clang-aarch64] Clang not found. Please install clang/clang++.")
endif ()

set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

find_program(AARCH64_GCC NAMES aarch64-linux-gnu-gcc)
if (NOT AARCH64_GCC)
	message(FATAL_ERROR "[toolchain/clang-aarch64] aarch64-linux-gnu-gcc not found.")
endif ()

if (NOT CMAKE_SYSROOT)
	execute_process(
			COMMAND ${AARCH64_GCC} --print-sysroot
			OUTPUT_VARIABLE _GCC_SYSROOT
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
	)
	if (_GCC_SYSROOT AND NOT "${_GCC_SYSROOT}" STREQUAL "/")
		set(CMAKE_SYSROOT "${_GCC_SYSROOT}")
	endif ()
endif ()

if (CMAKE_SYSROOT)
	set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
else ()
	set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")
endif ()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

find_program(LLD_BIN NAMES ld.lld-${LLVM_VERSION} ld.lld lld)
if (NOT LLD_BIN)
	message(FATAL_ERROR "[toolchain/clang-aarch64] LLD not found.")
endif ()

set(CMAKE_EXE_LINKER_FLAGS_INIT "--target=aarch64-linux-gnu -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--target=aarch64-linux-gnu -fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--target=aarch64-linux-gnu -fuse-ld=lld")

find_program(LLVM_AR_BIN NAMES llvm-ar-${LLVM_VERSION} llvm-ar)
find_program(LLVM_RANLIB_BIN NAMES llvm-ranlib-${LLVM_VERSION} llvm-ranlib)
if (LLVM_AR_BIN)
	set(CMAKE_AR "${LLVM_AR_BIN}" CACHE FILEPATH "")
	set(CMAKE_RANLIB "${LLVM_RANLIB_BIN}" CACHE FILEPATH "")
endif ()

message(STATUS "[toolchain/clang-aarch64] Cross GCC: ${AARCH64_GCC}")
message(STATUS "[toolchain/clang-aarch64] Sysroot: ${CMAKE_SYSROOT}")
message(STATUS "[toolchain/clang-aarch64] Linker: ${LLD_BIN}")
