set(TACHYON_CORE_SRCS
		src/arena.cpp
		src/shm.cpp
		src/star.cpp
		src/tachyon_c.cpp
		src/tachyon_rpc.cpp
		src/tachyon_star.cpp
		src/transport_uds.cpp
)

if (NOT EMSCRIPTEN)
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
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wasm_entry.cpp "")
	add_executable(tachyon_wasm ${CMAKE_CURRENT_BINARY_DIR}/wasm_entry.cpp)

	target_link_libraries(tachyon_wasm PRIVATE -Wl,--whole-archive tachyon -Wl,--no-whole-archive)
	target_link_options(tachyon_wasm PRIVATE
			"-sALLOW_MEMORY_GROWTH=1"
			"-sMODULARIZE=1"
			"-sEXPORT_NAME=TachyonCore"
			"-sSTRICT=1"
			"-sNO_EXIT_RUNTIME=1"
			"-sWASM_BIGINT=1"
			"-sFILESYSTEM=0"
			"--no-entry"
			"--minify=0"
			"--profiling-funcs"
			"-sEXPORTED_FUNCTIONS=_malloc,_free"
			"-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString"
	)

	set_target_properties(tachyon_wasm PROPERTIES
			OUTPUT_NAME "tachyon"
			SUFFIX ".js"
	)
endif ()
