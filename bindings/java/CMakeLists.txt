if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
	set(JAVA_NATIVE_ARCH "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
	set(JAVA_NATIVE_ARCH "arm64")
else ()
	set(JAVA_NATIVE_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif ()

string(TOLOWER ${CMAKE_SYSTEM_NAME} JAVA_NATIVE_OS)

set(JAVA_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/resources/native/${JAVA_NATIVE_OS}-${JAVA_NATIVE_ARCH}")

if (TARGET tachyon)
	add_custom_target(tachyon_java_resources ALL
			COMMAND ${CMAKE_COMMAND} -E make_directory "${JAVA_RESOURCES_DIR}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:tachyon>
			"${JAVA_RESOURCES_DIR}/$<TARGET_FILE_NAME:tachyon>"
			DEPENDS tachyon
			COMMENT "Packaging Tachyon FFM native library for ${JAVA_NATIVE_OS}-${JAVA_NATIVE_ARCH}"
	)
else ()
	message(WARNING "Target 'tachyon' not found. Cannot configure Java native resources copy.")
endif ()
