add_library(tachyon SHARED
		src/arena.cpp
		src/shm.cpp
		src/tachyon_c.cpp
		src/transport_uds.cpp
)

target_compile_options(tachyon PRIVATE
		${TACHYON_FLAGS}
		$<$<CONFIG:Release>:${TACHYON_RELEASE_FLAGS}>
		$<$<CONFIG:Debug>:${TACHYON_DEBUG_FLAGS}>
)

target_link_options(tachyon PRIVATE
		${LINK_OPTIONS}
		$<$<CONFIG:Release>:-flto>
)

set_target_properties(tachyon PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		C_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON
		OUTPUT_NAME "tachyon"
)

if (APPLE)
	set_target_properties(tachyon PROPERTIES INSTALL_RPATH "@loader_path")
else ()
	set_target_properties(tachyon PROPERTIES INSTALL_RPATH "$ORIGIN")
endif ()

target_include_directories(tachyon
		PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		PRIVATE src
)

if(UNIX AND NOT APPLE)
	target_link_libraries(tachyon PRIVATE rt)
	target_link_options(tachyon PRIVATE "-Wl,-z,defs" "-Wl,-z,now" "-Wl,-z,relro")
endif()

set(TACHYON_LIBRARY tachyon PARENT_SCOPE)
