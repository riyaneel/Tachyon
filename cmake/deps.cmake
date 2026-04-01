include_guard(GLOBAL)
include(FetchContent)

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
		googletest
		GIT_REPOSITORY https://github.com/google/googletest.git
		GIT_TAG v1.17.0
		GIT_SHALLOW TRUE
		GIT_PROGRESS FALSE
		SYSTEM
		OVERRIDE_FIND_PACKAGE
)

set(BUILD_MOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
set(gtest_force_shared_crt ON CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

if (TACHYON_SANITIZER STREQUAL "msan" AND TARGET gtest)
	target_compile_options(gtest PRIVATE ${TACHYON_DEBUG_FLAGS})
	target_compile_options(gtest_main PRIVATE ${TACHYON_DEBUG_FLAGS})
endif ()

if (TARGET gtest)
	target_compile_options(gtest PRIVATE -w)
	target_compile_options(gtest_main PRIVATE -w)
endif ()

FetchContent_Declare(
		benchmark
		GIT_REPOSITORY https://github.com/google/benchmark.git
		GIT_TAG v1.9.5
		GIT_SHALLOW TRUE
		GIT_PROGRESS FALSE
		SYSTEM
		OVERRIDE_FIND_PACKAGE
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE INTERNAL "")
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE INTERNAL "")
FetchContent_MakeAvailable(benchmark)

if (TARGET benchmark)
	target_compile_options(benchmark PRIVATE -w)
endif ()

FetchContent_Declare(
		dlpack
		GIT_REPOSITORY https://github.com/dmlc/dlpack.git
		GIT_TAG v1.3
		GIT_SHALLOW TRUE
		GIT_PROGRESS FALSE
		SYSTEM
)

FetchContent_MakeAvailable(dlpack)
set(TACHYON_DLPACK_INCLUDE_DIR "${dlpack_SOURCE_DIR}/include" CACHE PATH "DLPack include dir")

message(STATUS "[deps] GoogleTest  : v1.17.0 (FetchContent)")
message(STATUS "[deps] Benchmark   : v1.9.5  (FetchContent)")
message(STATUS "[deps] DLPack      : v1.3    (FetchContent)")
message(STATUS "[deps] DLPack inc  : ${TACHYON_DLPACK_INCLUDE_DIR}")
