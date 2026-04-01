include_guard(GLOBAL)

set(TACHYON_SANITIZER "asan_ubsan" CACHE STRING "Sanitizer preset: none | asan_ubsan | tsan | msan")
set_property(CACHE TACHYON_SANITIZER PROPERTY STRINGS none asan_ubsan tsan msan)

set(_tachyon_san_valid none asan_ubsan tsan msan)

if (NOT TACHYON_SANITIZER IN_LIST _tachyon_san_valid)
	message(FATAL_ERROR
			"[TachyonSanitizers] Invalid preset '${TACHYON_SANITIZER}'.\n"
			"Valid values: none | asan_ubsan | tsan | msan")
endif ()

if (TACHYON_SANITIZER STREQUAL "msan")
	if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		message(FATAL_ERROR
				"[TachyonSanitizers] msan requires Clang.\n"
				"Current compiler: ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER})\n"
				"Re-run cmake with: -DCMAKE_CXX_COMPILER=clang++")
	endif ()
endif ()

if (TACHYON_SANITIZER STREQUAL "tsan")
	if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		message(WARNING
				"[TachyonSanitizers] tsan: Clang recommended — "
				"GCC may produce false positives on unannotated futex primitives.")
	endif ()
endif ()

if (TACHYON_SANITIZER STREQUAL "asan_ubsan")
	set(TACHYON_DEBUG_FLAGS
			-O0
			-g3
			-fno-omit-frame-pointer
			-fsanitize=address,undefined
	)
	set(TACHYON_SAN_LINK_FLAGS
			$<$<CONFIG:Debug>:-fsanitize=address,undefined>
	)
elseif (TACHYON_SANITIZER STREQUAL "tsan")
	set(TACHYON_DEBUG_FLAGS
			-O1
			-g3
			-fno-omit-frame-pointer
			-fsanitize=thread
	)
	set(TACHYON_SAN_LINK_FLAGS
			$<$<CONFIG:Debug>:-fsanitize=thread>
	)
elseif (TACHYON_SANITIZER STREQUAL "msan")
	set(TACHYON_DEBUG_FLAGS
			-O1
			-g3
			-fno-omit-frame-pointer
			-fno-optimize-sibling-calls
			-fsanitize=memory
			-fsanitize-memory-track-origins=2
	)
	set(TACHYON_SAN_LINK_FLAGS
			$<$<CONFIG:Debug>:-fsanitize=memory>
	)
else ()
	set(TACHYON_DEBUG_FLAGS
			-O0
			-g3
			-fno-omit-frame-pointer
	)
	set(TACHYON_SAN_LINK_FLAGS "")
endif ()

function(tachyon_apply_sanitizers TARGET)
	if (TACHYON_SANITIZER STREQUAL "none")
		return()
	endif ()

	foreach (_flag IN LISTS TACHYON_DEBUG_FLAGS)
		target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Debug>:${_flag}>)
	endforeach ()

	if (TACHYON_SAN_LINK_FLAGS)
		target_link_options(${TARGET} PRIVATE ${TACHYON_SAN_LINK_FLAGS})
	endif ()
endfunction()

message(STATUS "Tachyon: Sanitizer -> ${TACHYON_SANITIZER}")
