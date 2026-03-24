cmake_minimum_required(VERSION 3.31)

project(Tachyon LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
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
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		message(STATUS "Tachyon: PGO phase 1 — GENERATE/Clang (profraw dir: ${TACHYON_PGO_DIR})")
		list(APPEND TACHYON_RELEASE_FLAGS -fprofile-instr-generate=${TACHYON_PGO_DIR}/default_%p.profraw)
		list(APPEND LINK_OPTIONS $<$<CONFIG:Release>:-fprofile-instr-generate=${TACHYON_PGO_DIR}/default_%p.profraw>)
	else ()
		message(STATUS "Tachyon: PGO phase 1 — GENERATE/GCC (gcda dir: ${TACHYON_PGO_DIR})")
		list(APPEND TACHYON_RELEASE_FLAGS -fprofile-generate=${TACHYON_PGO_DIR})
		list(APPEND LINK_OPTIONS $<$<CONFIG:Release>:-fprofile-generate=${TACHYON_PGO_DIR}>)
	endif ()
elseif (TACHYON_PGO_PHASE STREQUAL "USE")
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(TACHYON_PROFDATA "${TACHYON_PGO_DIR}/merged.profdata")
		if (NOT EXISTS "${TACHYON_PROFDATA}")
			message(FATAL_ERROR
					"PGO_PHASE=USE (Clang) but merged profile not found: ${TACHYON_PROFDATA}\n"
					"Run phase GENERATE first, then:\n"
					"  llvm-profdata merge -output=${TACHYON_PROFDATA} ${TACHYON_PGO_DIR}/*.profraw"
			)
		endif ()
		message(STATUS "Tachyon: PGO phase 2 — USE/Clang (profdata: ${TACHYON_PROFDATA})")
		list(APPEND TACHYON_RELEASE_FLAGS -fprofile-instr-use=${TACHYON_PROFDATA})
		list(APPEND LINK_OPTIONS $<$<CONFIG:Release>:-fprofile-instr-use=${TACHYON_PROFDATA}>)
	else ()
		if (NOT EXISTS "${TACHYON_PGO_DIR}")
			message(FATAL_ERROR
					"PGO_PHASE=USE (GCC) but profile directory does not exist: ${TACHYON_PGO_DIR}\n"
					"Run phase GENERATE first, then execute the benchmark."
			)
		endif ()
		message(STATUS "Tachyon: PGO phase 2 — USE/GCC (gcda dir: ${TACHYON_PGO_DIR})")
		list(APPEND TACHYON_RELEASE_FLAGS
				-fprofile-use=${TACHYON_PGO_DIR}
				-fprofile-correction
		)
		list(APPEND LINK_OPTIONS
				$<$<CONFIG:Release>:-fprofile-use=${TACHYON_PGO_DIR}>
				$<$<CONFIG:Release>:-fprofile-correction>
		)
	endif ()
elseif (NOT TACHYON_PGO_PHASE STREQUAL "")
	message(FATAL_ERROR "Invalid TACHYON_PGO_PHASE '${TACHYON_PGO_PHASE}'. Use GENERATE or USE.")
endif ()

add_subdirectory(core)
add_subdirectory(test)
add_subdirectory(benchmark)
add_subdirectory(bindings)

option(TACHYON_ENABLE_FUZZING "Build libFuzzer harnesses (Clang only)" OFF)

if (TACHYON_ENABLE_FUZZING)
	if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		message(FATAL_ERROR
				"TACHYON_ENABLE_FUZZING=ON requires Clang.\n"
				"Current compiler: ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER})\n"
				"libFuzzer is part of the Clang toolchain and is not available under GCC.\n"
				"Re-run cmake with: -DCMAKE_CXX_COMPILER=clang++"
		)
	endif ()

	if (TACHYON_PGO_PHASE STREQUAL "GENERATE")
		message(FATAL_ERROR
				"TACHYON_ENABLE_FUZZING=ON is incompatible with TACHYON_PGO_PHASE=GENERATE.\n"
				"PGO instrumentation and libFuzzer instrumentation conflict at link time."
		)
	endif ()

	message(STATUS "Tachyon: Fuzzing enabled (libFuzzer + ASan)")
	add_subdirectory(fuzz)
endif ()

message(STATUS "Tachyon: Build type -> ${CMAKE_BUILD_TYPE}")
