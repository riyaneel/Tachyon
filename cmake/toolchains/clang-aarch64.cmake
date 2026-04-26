set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if (NOT DEFINED LLVM_VERSION)
	set(LLVM_VERSION 21)
endif ()

set(CMAKE_C_COMPILER clang-${LLVM_VERSION})
set(CMAKE_CXX_COMPILER clang++-${LLVM_VERSION})

set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

find_program(AARCH64_GCC NAMES aarch64-linux-gnu-gcc)
if (NOT AARCH64_GCC)
	message(FATAL_ERROR "[toolchain/clang-aarch64] aarch64-linux-gnu-gcc not found.")
endif ()

execute_process(
		COMMAND ${AARCH64_GCC} --print-sysroot
		OUTPUT_VARIABLE _GCC_SYSROOT
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
)

if (EXISTS "/usr/aarch64-redhat-linux/sys-root/fc43/usr/include")
	set(_GCC_SYSROOT "/usr/aarch64-redhat-linux/sys-root/fc43")
endif ()

if (_GCC_SYSROOT AND EXISTS "${_GCC_SYSROOT}/usr/include")
	set(CMAKE_SYSROOT "${_GCC_SYSROOT}")
else ()
	message(FATAL_ERROR "[toolchain/clang-aarch64] Sysroot not found: (${AARCH64_GCC} --print-sysroot -> '${_GCC_SYSROOT}')")
endif ()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
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
