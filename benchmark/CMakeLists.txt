find_package(benchmark REQUIRED)

add_executable(tachyon_benchmark
		src/main.cpp
)

target_compile_options(tachyon_benchmark PRIVATE
		${TACHYON_FLAGS}
		$<$<CONFIG:Release>:${TACHYON_RELEASE_FLAGS}>
		$<$<CONFIG:Debug>:${TACHYON_DEBUG_FLAGS}>
)

target_link_options(tachyon_benchmark PRIVATE ${LINK_OPTIONS})

target_link_libraries(tachyon_benchmark
		PRIVATE
		tachyon
		benchmark::benchmark
)

target_compile_options(tachyon_benchmark PRIVATE ${COMPILE_OPTIONS})
target_link_options(tachyon_benchmark PRIVATE ${LINK_OPTIONS})

target_include_directories(tachyon_benchmark
		PRIVATE
		${CMAKE_SOURCE_DIR}/core/src
)

target_compile_definitions(tachyon_benchmark PRIVATE NDEBUG)
