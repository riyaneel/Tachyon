if (NOT DEFINED CMAKE_JS_VERSION)
	message(STATUS "Tachyon: Node.js bindings skipped (must be built via cmake-js/npm)")
	return()
endif ()

cmake_minimum_required(VERSION 3.31.0)
project(tachyon_node CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

execute_process(
		COMMAND node -p "require('node-addon-api').include"
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE NODE_ADDON_API_DIR
		OUTPUT_STRIP_TRAILING_WHITESPACE
)

string(REPLACE "\"" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")

add_library(tachyon_node SHARED
		src/native/tachyon_node.cpp
		src/native/_core_local/src/arena.cpp
		src/native/_core_local/src/shm.cpp
		src/native/_core_local/src/transport_uds.cpp
		src/native/_core_local/src/tachyon_c.cpp
)

set_target_properties(tachyon_node PROPERTIES
		PREFIX ""
		SUFFIX ".node"
)

target_include_directories(tachyon_node PRIVATE
		${CMAKE_JS_INC}
		${NODE_ADDON_API_DIR}
		src/native/_core_local/include
)

target_compile_definitions(tachyon_node PRIVATE NAPI_VERSION=8)
target_link_libraries(tachyon_node PRIVATE ${CMAKE_JS_LIB})

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(tachyon_node PRIVATE rt pthread)
endif ()
