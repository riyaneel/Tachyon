cmake_minimum_required(VERSION 3.31)

project(cpp_inference_producer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TACHYON_CORE "${CMAKE_SOURCE_DIR}/../../../core")

add_executable(producer src/main.cpp
		${TACHYON_CORE}/src/arena.cpp
		${TACHYON_CORE}/src/shm.cpp
		${TACHYON_CORE}/src/tachyon_c.cpp
		${TACHYON_CORE}/src/transport_uds.cpp
)

target_include_directories(producer PRIVATE ${TACHYON_CORE}/include)

target_compile_options(producer PRIVATE
		-O3 -march=native -mtune=native
		-fno-exceptions -fno-rtti
		-Wall -Wextra
		-fvisibility=hidden
)

if (UNIX AND NOT APPLE)
	target_link_libraries(producer PRIVATE rt)
endif ()