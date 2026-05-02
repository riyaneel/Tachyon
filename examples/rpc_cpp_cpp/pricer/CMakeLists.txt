cmake_minimum_required(VERSION 3.31)
project(tachyon_pricer CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(CORE_DIR "${ROOT_DIR}/core")

add_executable(tachyon_pricer
		src/main.cpp
		${CORE_DIR}/src/arena.cpp
		${CORE_DIR}/src/shm.cpp
		${CORE_DIR}/src/tachyon_c.cpp
		${CORE_DIR}/src/tachyon_rpc.cpp
		${CORE_DIR}/src/transport_uds.cpp
)

target_include_directories(tachyon_pricer PRIVATE ${CORE_DIR}/include)

target_compile_options(tachyon_pricer PRIVATE
		-O3 -march=native -mtune=native -funroll-loops
		-fno-exceptions -fno-rtti -fno-plt -fvisibility=hidden
		-Wall -Wextra -DNDEBUG
)

set_property(TARGET tachyon_pricer PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(tachyon_pricer PRIVATE rt pthread)
	target_link_options(tachyon_pricer PRIVATE -Wl,-z,now -Wl,-z,relro -flto)
endif ()
