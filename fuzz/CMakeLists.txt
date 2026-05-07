option(TACHYON_CFL_BUILD "Build fuzz targets for ClusterFuzzLite" OFF)

set(TACHYON_LIB_FUZZING_ENGINE "" CACHE STRING "libFuzzer link argument for CFL builds")

set(TACHYON_FUZZ_SANITIZER "asan" CACHE STRING "Sanitizer for local fuzz builds: asan | asan_ubsan | msan | tsan | none")
set_property(CACHE TACHYON_FUZZ_SANITIZER PROPERTY STRINGS asan asan_ubsan msan tsan none)

set(_valid_fuzz_san asan asan_ubsan msan tsan none)
if (NOT TACHYON_FUZZ_SANITIZER IN_LIST _valid_fuzz_san)
	message(FATAL_ERROR "[fuzz] Invalid TACHYON_FUZZ_SANITIZER '${TACHYON_FUZZ_SANITIZER}'")
endif ()

if (TACHYON_CFL_BUILD)
	if (TACHYON_LIB_FUZZING_ENGINE STREQUAL "")
		message(FATAL_ERROR "[fuzz] TACHYON_CFL_BUILD=ON requires TACHYON_LIB_FUZZING_ENGINE")
	endif ()

	set(FUZZ_FLAGS "")
	message(STATUS "[fuzz] CFL build: flags from environment, engine=${TACHYON_LIB_FUZZING_ENGINE}")
else ()
	if (TACHYON_FUZZ_SANITIZER STREQUAL "asan")
		set(_san -fsanitize=fuzzer,address)
	elseif (TACHYON_FUZZ_SANITIZER STREQUAL "asan_ubsan")
		set(_san -fsanitize=fuzzer,address,undefined)
	elseif (TACHYON_FUZZ_SANITIZER STREQUAL "msan")
		if (NOT EXISTS "${TACHYON_MSAN_LIBCXX_DIR}/include/c++/v1")
			message(FATAL_ERROR "[fuzz] TACHYON_FUZZ_SANITIZER=msan requires an MSan-instrumented libc++.")
		endif ()

		set(_san -fsanitize=fuzzer,memory)
	elseif (TACHYON_FUZZ_SANITIZER STREQUAL "tsan")
		set(_san -fsanitize=fuzzer,thread)
	elseif (TACHYON_FUZZ_SANITIZER STREQUAL "none")
		set(_san -fsanitize=fuzzer)
	endif ()

	message(STATUS "[fuzz] Sanitizer         : ${TACHYON_FUZZ_SANITIZER} (${_san})")

	set(FUZZ_FLAGS
			${_san}
			-fno-omit-frame-pointer
			-g
			-O1
			-fno-exceptions
			-fno-rtti
			-fvisibility=hidden
	)

	string(APPEND CMAKE_EXE_LINKER_FLAGS " ${_san}")
endif ()

add_library(tachyon_fuzz_core OBJECT
		${CMAKE_SOURCE_DIR}/core/src/arena.cpp
		${CMAKE_SOURCE_DIR}/core/src/shm.cpp
)

if (FUZZ_FLAGS)
	target_compile_options(tachyon_fuzz_core PRIVATE ${FUZZ_FLAGS})
endif ()

target_include_directories(tachyon_fuzz_core
		PUBLIC ${CMAKE_SOURCE_DIR}/core/include
		PRIVATE ${CMAKE_SOURCE_DIR}/core/src
)

set_target_properties(tachyon_fuzz_core PROPERTIES EXCLUDE_FROM_ALL ON)

# Target macro
macro(tachyon_fuzz_target TARGET_NAME SOURCE_FILE)
	add_executable(${TARGET_NAME} ${SOURCE_FILE})

	if (FUZZ_FLAGS)
		target_compile_options(${TARGET_NAME} PRIVATE ${FUZZ_FLAGS})
	endif ()

	if (TACHYON_CFL_BUILD)
		target_link_libraries(${TARGET_NAME} PRIVATE "${TACHYON_LIB_FUZZING_ENGINE}")
	endif ()

	target_include_directories(${TARGET_NAME}
			PRIVATE
			${CMAKE_SOURCE_DIR}/core/include
			${CMAKE_SOURCE_DIR}/core/src
	)

	target_link_libraries(${TARGET_NAME} PRIVATE tachyon_fuzz_core)

	if (UNIX AND NOT APPLE)
		target_link_libraries(${TARGET_NAME} PRIVATE rt)
	endif ()

	set_target_properties(${TARGET_NAME} PROPERTIES EXCLUDE_FROM_ALL ON)

	message(STATUS "[fuzz] Target registered: ${TARGET_NAME}")
endmacro()

# Fuzz targets
tachyon_fuzz_target(tachyon_fuzz_arena_rpc src/fuzz_arena_rpc.cpp)
tachyon_fuzz_target(tachyon_fuzz_arena_rx src/fuzz_arena_rx.cpp)
tachyon_fuzz_target(tachyon_fuzz_arena_rx_batch src/fuzz_arena_rx_batch.cpp)
tachyon_fuzz_target(tachyon_fuzz_arena_tx src/fuzz_arena_tx.cpp)
tachyon_fuzz_target(tachyon_fuzz_header_parser src/fuzz_header_parser.cpp)
tachyon_fuzz_target(tachyon_fuzz_shm_attach src/fuzz_shm_attach.cpp)
tachyon_fuzz_target(tachyon_fuzz_toctou src/fuzz_toctou.cpp)
