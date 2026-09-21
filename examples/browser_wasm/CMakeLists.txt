add_executable(tachyon_example echo/echo.cpp)

target_link_libraries(tachyon_example PRIVATE tachyon)

set(TACHYON_EXAMPLE_EXPORTS ${TACHYON_WASM_EXPORTS} _tachyon_browser_echo_once)
list(JOIN TACHYON_EXAMPLE_EXPORTS "," TACHYON_EXAMPLE_EXPORTS_ARG)

target_link_options(tachyon_example PRIVATE
		${TACHYON_WASM_LINK_OPTIONS}
		"-sEXPORT_NAME=TachyonExample"
		"-sEXPORTED_FUNCTIONS=${TACHYON_EXAMPLE_EXPORTS_ARG}"
)

set_target_properties(tachyon_example PROPERTIES
		OUTPUT_NAME "tachyon_example"
		SUFFIX ".js"
)

set(TACHYON_EXAMPLE_STAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pkg")
add_custom_target(tachyon_example_stage
		COMMAND ${CMAKE_COMMAND} -E make_directory "${TACHYON_EXAMPLE_STAGE_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:tachyon_example>/tachyon_example.js"
		"$<TARGET_FILE_DIR:tachyon_example>/tachyon_example.wasm"
		"${TACHYON_EXAMPLE_STAGE_DIR}"
		DEPENDS tachyon_example
		COMMENT "Staging demo module into examples/browser_wasm/pkg"
		VERBATIM
)
