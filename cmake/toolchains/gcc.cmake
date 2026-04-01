set(GCC_VERSION "14" CACHE STRING "GCC version to use")

find_program(GCC_C_BIN NAMES gcc-${GCC_VERSION} gcc REQUIRED)
find_program(GCC_CXX_BIN NAMES g++-${GCC_VERSION} g++ REQUIRED)

set(CMAKE_C_COMPILER "${GCC_C_BIN}" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${GCC_CXX_BIN}" CACHE FILEPATH "")

find_program(GCC_AR_BIN NAMES gcc-ar-${GCC_VERSION} gcc-ar)
find_program(GCC_RANLIB_BIN NAMES gcc-ranlib-${GCC_VERSION} gcc-ranlib)

if (GCC_AR_BIN)
	set(CMAKE_AR "${GCC_AR_BIN}" CACHE FILEPATH "")
	set(CMAKE_RANLIB "${GCC_RANLIB_BIN}" CACHE FILEPATH "")
endif ()

find_program(GOLD_BIN NAMES ld.gold gold)
if (GOLD_BIN)
	set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=gold")
	set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=gold")
	set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=gold")
endif ()

set(TACHYON_LTO "fat" CACHE STRING "LTO mode: fat | thin | off")

message(STATUS "[toolchain/gcc] GCC ${GCC_VERSION} — LTO=${TACHYON_LTO}")
