add_library(tachyon_jni SHARED src/native/tachyon_jni.cpp)

if (TARGET tachyon)
	target_link_libraries(tachyon_jni PRIVATE tachyon)
else ()
	target_include_directories(tachyon_jni PRIVATE src/native/_core_local/include)
	target_sources(tachyon_jni PRIVATE
			src/native/_core_local/src/tachyon_c.cpp
			src/native/_core_local/src/arena.cpp
			src/native/_core_local/src/shm.cpp
			src/native/_core_local/src/transport_uds.cpp
	)
endif ()

target_include_directories(tachyon_jni PRIVATE ${JNI_INCLUDE_DIRS})

set_target_properties(tachyon_jni PROPERTIES PREFIX "lib")
tachyon_set_compile_options(tachyon_jni)
