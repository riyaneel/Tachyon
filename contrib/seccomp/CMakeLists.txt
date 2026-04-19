if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(STATUS "Tachyon seccomp is Linux only")
	return()
endif ()

enable_language(C)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
	pkg_check_modules(SECCOMP QUIET libseccomp)
endif ()

if (NOT SECCOMP_FOUND)
	message(WARNING "Tachyon seccomp: libseccomp not found")
	return()
endif ()

message(STATUS "Tachyon seccomp: libseccomp ${SECCOMP_VERSION} found")

function(tachyon_bpf_generator TARGET_NAME SOURCE)
	add_executable(${TARGET_NAME} ${SOURCE})
	set_target_properties(${TARGET_NAME} PROPERTIES
			C_STANDARD 11
			EXCLUDE_FROM_ALL ON
	)

	target_include_directories(${TARGET_NAME} PRIVATE ${SECCOMP_INCLUDE_DIRS})
	target_link_libraries(${TARGET_NAME} PRIVATE ${SECCOMP_LIBRARIES})
	target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Werror)
endfunction()

tachyon_bpf_generator(tachyon_hotpath tachyon_hotpath.bpf.c)
tachyon_bpf_generator(tachyon_full tachyon_full.bpf.c)
