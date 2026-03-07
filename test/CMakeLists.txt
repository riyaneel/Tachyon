find_package(GTest REQUIRED)

add_executable(tachyon_tests
		src/test_main.cpp
)

target_compile_options(tachyon_tests PRIVATE
		${TACHYON_FLAGS}
		$<$<CONFIG:Release>:${TACHYON_RELEASE_FLAGS}>
		$<$<CONFIG:Debug>:${TACHYON_DEBUG_FLAGS}>
)

target_link_options(tachyon_tests PRIVATE ${LINK_OPTIONS})

target_link_libraries(tachyon_tests
		PRIVATE
		tachyon
		GTest::gtest
		GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tachyon_tests)

target_include_directories(tachyon_tests
		PRIVATE
		${CMAKE_SOURCE_DIR}/core/src
)
