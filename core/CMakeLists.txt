add_library(tachyon SHARED

)

set_target_properties(tachyon PROPERTIES
		CXX_VISIBILITY_PRESET hidden
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

if (UNIX AND NOT APPLE)
	target_link_libraries(tachyon PRIVATE rt)
endif ()

set(TACHYON_LIBRARY tachyon PARENT_SCOPE)
