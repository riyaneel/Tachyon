set(LLVM_VERSION "21" CACHE STRING "LLVM version to use")

find_program(CLANG_C_BIN NAMES clang-${LLVM_VERSION} clang REQUIRED DOC "Clang C compiler (version ${LLVM_VERSION})")
find_program(CLANG_CXX_BIN NAMES clang++-${LLVM_VERSION} clang++ REQUIRED DOC "Clang CXX compiler (version ${LLVM_VERSION})")

set(CMAKE_C_COMPILER "${CLANG_C_BIN}" CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${CLANG_CXX_BIN}" CACHE FILEPATH "C++ compiler")

find_program(LLD_BIN NAMES ld.lld-${LLVM_VERSION} lld DOC "LLD linker (version ${LLVM_VERSION})")
if (LLD_BIN)
	set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
	set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
	set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
	message(STATUS "[toolchain/clang] Linker: LLD found at ${LLD_BIN}.")
else ()
	message(WARNING "[toolchain/clang] LLD not found, falling back to system linker.")
endif ()

find_program(LLVM_AR_BIN NAMES llvm-ar-${LLVM_VERSION} llvm-ar)
find_program(LLVM_NM_BIN NAMES llvm-nm-${LLVM_VERSION} llvm-nm)
find_program(LLVM_RANLIB_BIN NAMES llvm-ranlib-${LLVM_VERSION} llvm-ranlib)

if (LLVM_AR_BIN)
	set(CMAKE_AR "${LLVM_AR_BIN}" CACHE FILEPATH "")
	set(CMAKE_NM "${LLVM_NM_BIN}" CACHE FILEPATH "")
	set(CMAKE_RANLIB "${LLVM_RANLIB_BIN}" CACHE FILEPATH "")
	message(STATUS "[toolchain/clang] AR: ${LLVM_AR_BIN}")
endif ()

set(TACHYON_LTO "fat" CACHE STRING "LTO mode: fat | thin | off")

message(STATUS "[toolchain/clang] Clang ${LLVM_VERSION} — LTO=${TACHYON_LTO}")
