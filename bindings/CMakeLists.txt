find_package(Python3 COMPONENTS Development)
if (Python3_FOUND)
	add_library(tachyon_python STATIC python/src/tachyon/_tachyon.cpp)
	target_link_libraries(tachyon_python PRIVATE tachyon Python3::Python)
	set_target_properties(tachyon_python PROPERTIES
			EXCLUDE_FROM_ALL ON
			CXX_STANDARD 23
	)
endif ()
