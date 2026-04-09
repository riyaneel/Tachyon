find_package(Python3 COMPONENTS Development)
if (Python3_FOUND)
	add_library(tachyon_python STATIC python/src/tachyon/_tachyon.cpp)
	target_link_libraries(tachyon_python PRIVATE tachyon Python3::Python)
	tachyon_set_compile_options(tachyon_python)
	set_target_properties(tachyon_python PROPERTIES
			EXCLUDE_FROM_ALL ON
			CXX_STANDARD 23
	)
endif ()

find_package(Java COMPONENTS Development)
find_package(JNI)

if (Java_FOUND AND JNI_FOUND)
	add_subdirectory(java)
else ()
	message(STATUS "Java Development or JNI not found - skipping Java bindings")
endif ()

add_subdirectory(node)
