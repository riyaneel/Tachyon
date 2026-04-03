#include <jni.h>

#include <tachyon.h>

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
	JNIEnv *env;
	if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_21) != JNI_OK) {
		return JNI_ERR;
	}

	(void)reserved;

	return JNI_VERSION_21;
}
