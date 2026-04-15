find_package(Threads REQUIRED)

add_executable(tachyon_bench_intra src/bench_intra.cpp)
tachyon_set_compile_options(tachyon_bench_intra)
target_link_options(tachyon_bench_intra PRIVATE ${TACHYON_LINK_OPTIONS})
target_link_libraries(tachyon_bench_intra
		PRIVATE tachyon benchmark::benchmark Threads::Threads
)
target_include_directories(tachyon_bench_intra
		PRIVATE ${CMAKE_SOURCE_DIR}/core/src
)

add_executable(tachyon_bench_inter src/bench_inter.cpp)
tachyon_set_compile_options(tachyon_bench_inter)
target_link_options(tachyon_bench_inter PRIVATE ${TACHYON_LINK_OPTIONS})
target_link_libraries(tachyon_bench_inter
		PRIVATE tachyon Threads::Threads
)
target_include_directories(tachyon_bench_inter
		PRIVATE ${CMAKE_SOURCE_DIR}/core/src
)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
	pkg_check_modules(ZMQ QUIET libzmq)
endif ()

if (ZMQ_FOUND)
	message(STATUS "Tachyon bench: ZeroMQ baseline enabled (${ZMQ_VERSION})")
	add_executable(tachyon_bench_zmq src/bench_zmq.cpp)
	tachyon_set_compile_options(tachyon_bench_zmq)
	target_link_options(tachyon_bench_zmq PRIVATE ${TACHYON_LINK_OPTIONS})
	target_link_libraries(tachyon_bench_zmq
			PRIVATE tachyon benchmark::benchmark Threads::Threads ${ZMQ_LIBRARIES}
	)
	target_include_directories(tachyon_bench_zmq PRIVATE
			${ZMQ_INCLUDE_DIRS}
			${CMAKE_SOURCE_DIR}/core/src
	)
	target_compile_definitions(tachyon_bench_zmq PRIVATE TACHYON_BENCH_ZMQ=1)
else ()
	message(STATUS "Tachyon bench: ZeroMQ not found - baseline disabled (install libzmq-dev)")
endif ()
