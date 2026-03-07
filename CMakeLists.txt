cmake_minimum_required(VERSION 3.31)

project(Tachyon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMPILE_OPTIONS
		-Wall -Wextra -Werror -Wpedantic -Wshadow
		-Wnon-virtual-dtor -Wcast-align -Wunused
		-Woverloaded-virtual -Wconversion -Wsign-conversion
		-fPIC -fvisibility=hidden
		$<$<CONFIG:Release>:-O3 -march=native -mtune=native -funroll-loops -fno-exceptions -fno-rtti -flto=thin -fno-plt>
		$<$<CONFIG:Debug>:-O0 -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer>
)

set(LINK_OPTIONS
		$<$<CONFIG:Release>:-flto=thin>
		$<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)

add_compile_options(${COMPILE_OPTIONS})
add_link_options(${LINK_OPTIONS})

if (NOT APPLE)
	add_link_options("-Wl,-z,defs" "-Wl,-z,now" "-Wl,-z,relro")
endif ()

add_subdirectory(core)

message(STATUS "Tachyon: Build type -> ${CMAKE_BUILD_TYPE}")