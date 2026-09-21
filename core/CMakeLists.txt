set(TACHYON_CORE_SRCS
		src/arena.cpp
		src/shm.cpp
		src/tachyon_c.cpp
		src/transport_uds.cpp
)

if (NOT EMSCRIPTEN)
	list(APPEND TACHYON_CORE_SRCS
			src/star.cpp
			src/tachyon_rpc.cpp
			src/tachyon_star.cpp
	)
	add_library(tachyon SHARED ${TACHYON_CORE_SRCS})
else ()
	add_library(tachyon STATIC ${TACHYON_CORE_SRCS})
endif ()

tachyon_set_compile_options(tachyon)
if (NOT EMSCRIPTEN)
	target_link_options(tachyon PRIVATE ${TACHYON_LINK_OPTIONS})
endif ()

set_target_properties(tachyon PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		C_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON
		OUTPUT_NAME "tachyon"
)

if (NOT EMSCRIPTEN)
	if (APPLE)
		set_target_properties(tachyon PROPERTIES INSTALL_RPATH "@loader_path")
	else ()
		set_target_properties(tachyon PROPERTIES INSTALL_RPATH "$ORIGIN")
	endif ()

	if (UNIX AND NOT APPLE)
		target_link_libraries(tachyon PRIVATE rt)
		target_link_options(tachyon PRIVATE
				$<$<NOT:$<CONFIG:Debug>>:-Wl,-z,defs>
				"-Wl,-z,now"
				"-Wl,-z,relro"
		)
	endif ()
endif ()

target_include_directories(tachyon
		PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		PRIVATE src
)

set(TACHYON_LIBRARY tachyon PARENT_SCOPE)

if (EMSCRIPTEN)
	set(TACHYON_WASM_EXPORTS
			_malloc
			_free
			_tachyon_bus_listen
			_tachyon_bus_destroy
			_tachyon_bus_get_shm_ptr
			_tachyon_bus_set_polling_mode
			_tachyon_acquire_tx
			_tachyon_commit_tx
			_tachyon_rollback_tx
			_tachyon_acquire_rx
			_tachyon_commit_rx
			_tachyon_acquire_rx_batch
			_tachyon_commit_rx_batch
			_tachyon_flush
			_tachyon_get_state
	)

	# Module shape shared by every WASM module built from this core.
	set(TACHYON_WASM_LINK_OPTIONS
			"-sALLOW_MEMORY_GROWTH=1"
			"-sABORTING_MALLOC=0"
			"-sMALLOC=emmalloc"
			"-sMODULARIZE=1"
			"-sEXPORT_ES6=1"
			"-sENVIRONMENT=web,worker"
			"-sSTRICT=1"
			"-sNO_EXIT_RUNTIME=1"
			"-sWASM_BIGINT=1"
			"-sFILESYSTEM=0"
			"--no-entry"
			"--minify=0"
			"--profiling-funcs"
			"-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,HEAPU8,HEAPU32"
	)

	set(TACHYON_WASM_EXPORTS ${TACHYON_WASM_EXPORTS} PARENT_SCOPE)
	set(TACHYON_WASM_LINK_OPTIONS ${TACHYON_WASM_LINK_OPTIONS} PARENT_SCOPE)

	# The shipped browser module is never built under a sanitizer
	if (TACHYON_SANITIZER STREQUAL "none")
		file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wasm_entry.cpp "")
		add_executable(tachyon_wasm ${CMAKE_CURRENT_BINARY_DIR}/wasm_entry.cpp)

		list(JOIN TACHYON_WASM_EXPORTS "," TACHYON_WASM_EXPORTS_ARG)

		target_link_libraries(tachyon_wasm PRIVATE tachyon)
		target_link_options(tachyon_wasm PRIVATE
				${TACHYON_WASM_LINK_OPTIONS}
				"-sEXPORT_NAME=TachyonCore"
				"-sEXPORTED_FUNCTIONS=${TACHYON_WASM_EXPORTS_ARG}"
		)

		set_target_properties(tachyon_wasm PROPERTIES
				OUTPUT_NAME "tachyon"
				SUFFIX ".js"
		)

		set(TACHYON_WASM_STAGE_DIR "${CMAKE_SOURCE_DIR}/bindings/js/src/ts/wasm")
		add_custom_target(tachyon_wasm_stage
				COMMAND ${CMAKE_COMMAND} -E make_directory "${TACHYON_WASM_STAGE_DIR}"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE_DIR:tachyon_wasm>/tachyon.js"
				"$<TARGET_FILE_DIR:tachyon_wasm>/tachyon.wasm"
				"${TACHYON_WASM_STAGE_DIR}"
				DEPENDS tachyon_wasm
				COMMENT "Staging WASM core into bindings/js/src/ts/wasm"
				VERBATIM
		)
	endif ()
endif ()
