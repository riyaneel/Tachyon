enable_testing()

set(TACHYON_TEST_SRCS
		src/test_arena.cpp
		src/test_shm.cpp
)
if (NOT EMSCRIPTEN)
	list(APPEND TACHYON_TEST_SRCS
			src/test_rpc.cpp
			src/test_star.cpp
	)
else ()
	list(APPEND TACHYON_TEST_SRCS
			src/test_wasm.cpp
			src/test_fuzz_replay.cpp
	)

	set(TACHYON_FUZZ_REPLAY_CASES arena_rx arena_rx_batch arena_tx header_parser toctou)
	foreach (case IN LISTS TACHYON_FUZZ_REPLAY_CASES)
		set(src "${CMAKE_SOURCE_DIR}/fuzz/src/fuzz_${case}.cpp")
		set_source_files_properties(${src} PROPERTIES
				COMPILE_DEFINITIONS "LLVMFuzzerTestOneInput=tachyon_replay_${case}"
		)

		list(APPEND TACHYON_TEST_SRCS ${src})
	endforeach ()
endif ()

add_executable(tachyon_tests ${TACHYON_TEST_SRCS})

if (NOT EMSCRIPTEN)
	target_link_options(tachyon_tests PRIVATE ${TACHYON_LINK_OPTIONS})
else ()
	target_compile_definitions(tachyon_tests PRIVATE
			TACHYON_CORPUS_DIR="${CMAKE_SOURCE_DIR}/fuzz/corpus"
	)

	target_link_options(tachyon_tests PRIVATE
			"-sENVIRONMENT=node"
			"-sNODERAWFS=1"
			"-sALLOW_MEMORY_GROWTH=1"
			"-sABORTING_MALLOC=0"
			"-sEXIT_RUNTIME=1"
			"-sSTACK_SIZE=1MB"
			"-sWASM_BIGINT=1"
			"--profiling-funcs"
	)

	if (NOT TACHYON_SANITIZER STREQUAL "none")
		target_link_options(tachyon_tests PRIVATE
				${TACHYON_SAN_LINK_FLAGS}
				"-sINITIAL_MEMORY=512MB"
				"-sSTACK_SIZE=4MB"
		)
	endif ()
endif ()

target_link_libraries(tachyon_tests
		PRIVATE
		tachyon
		gtest
		gtest_main
)

include(GoogleTest)
gtest_discover_tests(tachyon_tests DISCOVERY_MODE PRE_TEST)

target_include_directories(tachyon_tests BEFORE PRIVATE
		${CMAKE_SOURCE_DIR}/core/src
)

target_include_directories(tachyon_tests SYSTEM BEFORE PRIVATE
		${googletest_SOURCE_DIR}/googletest/include
)
