cmake_minimum_required(VERSION 3.31)

project(Tachyon LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TACHYON_FLAGS
		-Wall -Wextra -Werror -Wpedantic -Wshadow
		-Wnon-virtual-dtor -Wcast-align -Wunused
		-Woverloaded-virtual -Wconversion -Wsign-conversion
		-fPIC -fvisibility=hidden
)

set(TACHYON_RELEASE_FLAGS
		-O3 -march=native -mtune=native -funroll-loops
		-fno-exceptions -fno-rtti -flto -fno-plt
)

set(TACHYON_DEBUG_FLAGS
		-O0 -g3 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
)

set(LINK_OPTIONS
		$<$<CONFIG:Release>:-flto>
		$<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)

set(TACHYON_PGO_PHASE "" CACHE STRING "PGO phase: GENERATE or USE (empty = disabled)")

set(TACHYON_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory for PGO profile data")

if (TACHYON_PGO_PHASE STREQUAL "GENERATE")
	message(STATUS "Tachyon: PGO phase 1 — GENERATE (profile dir: ${TACHYON_PGO_DIR})")
	list(APPEND TACHYON_RELEASE_FLAGS -fprofile-generate=${TACHYON_PGO_DIR})
	list(APPEND LINK_OPTIONS $<$<CONFIG:Release>:-fprofile-generate=${TACHYON_PGO_DIR}>)
elseif (TACHYON_PGO_PHASE STREQUAL "USE")
	if (NOT EXISTS "${TACHYON_PGO_DIR}")
		message(FATAL_ERROR "PGO_PHASE=USE but profile directory does not exist: ${TACHYON_PGO_DIR}\n"
				"Run phase GENERATE first, then execute the benchmark.")
	endif ()
	message(STATUS "Tachyon: PGO phase 2 — USE (profile dir: ${TACHYON_PGO_DIR})")
	list(APPEND TACHYON_RELEASE_FLAGS
			-fprofile-use=${TACHYON_PGO_DIR}
			-fprofile-correction       # tolerate minor counter mismatches from LTO+PGO
			-Wno-missing-profile       # expected: some TUs have no counters
	)
	list(APPEND LINK_OPTIONS
			$<$<CONFIG:Release>:-fprofile-use=${TACHYON_PGO_DIR}>
			$<$<CONFIG:Release>:-fprofile-correction>
	)
elseif (NOT TACHYON_PGO_PHASE STREQUAL "")
	message(FATAL_ERROR "Invalid TACHYON_PGO_PHASE '${TACHYON_PGO_PHASE}'. Use GENERATE or USE.")
endif ()

add_subdirectory(core)
add_subdirectory(test)
add_subdirectory(benchmark)
add_subdirectory(bindings)

message(STATUS "Tachyon: Build type -> ${CMAKE_BUILD_TYPE}")
