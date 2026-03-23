set(FUZZ_FLAGS
		-fsanitize=fuzzer,address
		-fno-omit-frame-pointer
		-g
		-O1
		-fno-exceptions
		-fno-rtti
		-fvisibility=hidden
)

set(FUZZ_LINK_FLAGS
		-fsanitize=fuzzer,address
)

add_library(tachyon_fuzz_core OBJECT
		${CMAKE_SOURCE_DIR}/core/src/arena.cpp
		${CMAKE_SOURCE_DIR}/core/src/shm.cpp
)

target_compile_options(tachyon_fuzz_core PRIVATE ${FUZZ_FLAGS})

target_include_directories(tachyon_fuzz_core
		PUBLIC ${CMAKE_SOURCE_DIR}/core/include
		PRIVATE ${CMAKE_SOURCE_DIR}/core/src
)

set_target_properties(tachyon_fuzz_core PROPERTIES EXCLUDE_FROM_ALL ON)

# Helper macro — each fuzz target links tachyon_fuzz_core + rt (Linux)
macro(tachyon_fuzz_target TARGET_NAME SOURCE_FILE)
	add_executable(${TARGET_NAME} ${SOURCE_FILE})

	target_compile_options(${TARGET_NAME} PRIVATE ${FUZZ_FLAGS})
	target_link_options(${TARGET_NAME} PRIVATE ${FUZZ_LINK_FLAGS})

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

	message(STATUS "Tachyon fuzz target: ${TARGET_NAME}")
endmacro()

# Fuzz targets
tachyon_fuzz_target(tachyon_fuzz_header_parser src/fuzz_header_parser.cpp)
tachyon_fuzz_target(tachyon_fuzz_arena_rx src/fuzz_arena_rx.cpp)
tachyon_fuzz_target(tachyon_fuzz_arena_rx_batch src/fuzz_arena_rx_batch.cpp)
