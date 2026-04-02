add_executable(tachyon_top
		src/main.cpp
)

tachyon_set_compile_options(tachyon_top)
target_link_options(tachyon_top PRIVATE ${TACHYON_LINK_OPTIONS})

target_include_directories(tachyon_top PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

target_link_libraries(tachyon_top
		PRIVATE
		ftxui::component
		ftxui::dom
		ftxui::screen
)

if (CMAKE_SYSTEM_NAME STREQUAL "linux")
	target_link_libraries(tachyon_top PRIVATE atomic)
endif ()