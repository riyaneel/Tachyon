include_guard(GLOBAL)

set(TACHYON_FLAGS
		-Wall -Wextra -Werror -Wpedantic
		-Wshadow -Wnon-virtual-dtor -Wcast-align
		-Wunused -Woverloaded-virtual
		-Wconversion -Wsign-conversion
		-fPIC -fvisibility=hidden
)

set(TACHYON_RELEASE_FLAGS
		-O3 -march=native -mtune=native
		-funroll-loops
		-fno-exceptions -fno-rtti
		-fno-plt
)

set(TACHYON_DEBUG_FLAGS
		-O0 -g3 -fno-omit-frame-pointer
)

if (TACHYON_LTO STREQUAL "fat")
	list(APPEND TACHYON_RELEASE_FLAGS -flto)
elseif (TACHYON_LTO STREQUAL "thin")
	list(APPEND TACHYON_RELEASE_FLAGS -flto=thin)
endif ()

function(tachyon_set_compile_options TARGET)
	target_compile_options(${TARGET} PRIVATE
			${TACHYON_FLAGS}
			$<$<CONFIG:Release>:${TACHYON_RELEASE_FLAGS}>
			$<$<CONFIG:Debug>:${TACHYON_DEBUG_FLAGS}>
	)
endfunction()
