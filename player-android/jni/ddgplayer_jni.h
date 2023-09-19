#include <jni.h>
#include <stdint.h>

JNIEXPORT JavaVM *get_jni_jvm(void);
JNIEXPORT JNIEnv *get_jni_env(void);
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved);

#ifdef __cplusplus
extern "C" {
#endif

void JniAttachCurrentThread();
void JniDetachCurrentThread();
void *JniRequestWinObj(void *data);
void JniReleaseWinObj(void *data);
void JniPostMessage(void *extra, int32_t msg, void *param);

#ifdef __cplusplus
}
#endif
