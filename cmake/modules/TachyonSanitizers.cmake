include_guard(GLOBAL)

set(TACHYON_SANITIZER "asan_ubsan" CACHE STRING "Sanitizer preset: none | asan_ubsan | tsan")
set_property(CACHE TACHYON_SANITIZER PROPERTY STRINGS none asan_ubsan tsan)

set(_tachyon_san_valid none asan_ubsan tsan)

if (NOT TACHYON_SANITIZER IN_LIST _tachyon_san_valid)
	message(FATAL_ERROR
			"[TachyonSanitizers] Invalid preset '${TACHYON_SANITIZER}'.\n"
			"Valid values: none | asan_ubsan | tsan")
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
