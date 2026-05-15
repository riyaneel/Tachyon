if (DEFINED ENV{EMSDK})
	set(EMSDK_ROOT "$ENV{EMSDK}")
else ()
	set(EMSDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.emsdk")
endif ()

set(EMSCRIPTEN_TOOLCHAIN "${EMSDK_ROOT}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

if (NOT EXISTS "${EMSCRIPTEN_TOOLCHAIN}")
	message(FATAL_ERROR
			"[toolchain/emscripten] Toolchain not found at: ${EMSCRIPTEN_TOOLCHAIN}\n"
			"Run: bash ci/setup/install_emsdk.sh"
	)
endif ()

include("${EMSCRIPTEN_TOOLCHAIN}")

message(STATUS "[toolchain/emscripten] Loaded toolchain from: ${EMSCRIPTEN_TOOLCHAIN}")
