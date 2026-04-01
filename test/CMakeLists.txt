enable_testing()

find_package(GTest REQUIRED)

add_executable(tachyon_tests
		src/test_arena.cpp
		src/test_shm.cpp
)

tachyon_set_compile_options(tachyon_tests)
target_link_options(tachyon_tests PRIVATE ${LINK_OPTIONS})
target_link_libraries(tachyon_tests
		PRIVATE
		tachyon
		GTest::gtest
		GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tachyon_tests DISCOVERY_MODE PRE_TEST)

target_include_directories(tachyon_tests
		PRIVATE
		${CMAKE_SOURCE_DIR}/core/src
)
