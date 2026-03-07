cmake_minimum_required(VERSION 3.31)

project(Tachyon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TACHYON_FLAGS
		-Wall -Wextra -Werror -Wpedantic -Wshadow
		-Wnon-virtual-dtor -Wcast-align -Wunused
		-Woverloaded-virtual -Wconversion -Wsign-conversion
		-fPIC -fvisibility=hidden
)

set(TACHYON_RELEASE_FLAGS
		-O3 -march=native -mtune=native -funroll-loops
		-fno-exceptions -fno-rtti -flto -fno-plt
)

set(TACHYON_DEBUG_FLAGS
		-O0 -g3 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
)

set(LINK_OPTIONS
		$<$<CONFIG:Release>:-flto>
		$<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)

add_subdirectory(core)
add_subdirectory(test)
add_subdirectory(benchmark)

message(STATUS "Tachyon: Build type -> ${CMAKE_BUILD_TYPE}")
