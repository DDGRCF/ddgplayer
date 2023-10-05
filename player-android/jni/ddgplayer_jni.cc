#include "ddgplayer_jni.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "adev.h"
#include "ffplayer.h"
#include "vdev.h"

extern "C" int av_jni_set_java_vm(void *vm, void *log_ctx);

static jstring JNICALL nativeLoadTest(JNIEnv *env, jobject obj) {
  const char *infos =
      "Hello From JNI! More Infos:\n"
      "nativeOpen: the native api to open media player\n"
      "nativeClose: the native api to close media player\n"
      "nativePlay: the native api to play the media file\n"
      "nativePause: the native api to pause the media file\n"
      "nativeSeek: the native api to seek the stream position\n"
      "nativeSetParam: the native api to set param of media player\n"
      "nativeGetParam: the native api to get param of media player\n"
      "nativeSetDisplaySurface: the native api to set display surface";
  jstring javaString = env->NewStringUTF(infos);
  return javaString;
}

static jlong JNICALL nativeOpen(JNIEnv *env, jobject obj, jstring url,
                                jobject jsurface, jint w, jint h,
                                jstring params) {
  DO_USE_VAR(jsurface);
  DO_USE_VAR(w);
  DO_USE_VAR(h);
  PlayerInitParams playerparams;
  memset(&playerparams, 0, sizeof(playerparams));
  if (params != NULL) {
    char *strparams = (char *)env->GetStringUTFChars(params, NULL);
    player_load_params(&playerparams, strparams);
    env->ReleaseStringUTFChars(params, strparams);
  }
  const char *strurl = env->GetStringUTFChars(url, NULL);
  jlong hplayer = (jlong)player_open((char *)url, obj, &playerparams);
  env->ReleaseStringUTFChars(url, strurl);
  return hplayer;
}

static void nativeClose(JNIEnv *env, jobject obj, jlong hplayer) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_close((void *)hplayer);
}

static void JNICALL nativePlay(JNIEnv *env, jobject obj, jlong hplayer) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_play((void *)hplayer);
}

static void JNICALL nativePause(JNIEnv *env, jobject obj, jlong hplayer) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_pause((void *)hplayer);
}

static void JNICALL nativeSeek(JNIEnv *env, jobject obj, jlong hplayer,
                               jlong ms) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_seek((void *)hplayer, ms, 0); // TODO(@ddg): 探索一下里面干了什么事情
}

static void JNICALL nativeSetParam(JNIEnv *env, jobject obj, jlong hplayer,
                                   jint id, jlong value) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_setparam((void *)hplayer, id, &value);
}

static jlong JNICALL nativeGetParam(JNIEnv *env, jobject obj, jlong hplayer,
                                    jint id) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  jlong value = 0;
  player_getparam((void *)hplayer, id, &value);
  return value;
}

static void JNICALL nativeSetDisplaySurface(JNIEnv *env, jobject obj,
                                            jlong hplayer, jobject surface) {
  DO_USE_VAR(env);
  DO_USE_VAR(obj);
  player_setparam((void *)hplayer, PARAM_RENDER_VDEV_WIN, surface);
}

static JavaVM *g_jvm = NULL;

static const JNINativeMethod g_methods[] = {
    {"nativeLoadTest", "()Ljava/lang/String;", (void *)nativeLoadTest},
    {"nativeOpen",
     "(Ljava/lang/String;Ljava/lang/Object;IILjava/lang/String;)J",
     (void *)nativeOpen},
    {"nativeClose", "(J)V", (void *)nativeClose},
    {"nativePlay", "(J)V", (void *)nativePlay},
    {"nativePause", "(J)V", (void *)nativePause},
    {"nativeSeek", "(JJ)V", (void *)nativeSeek},
    {"nativeSetParam", "(JIJ)V", (void *)nativeSetParam},
    {"nativeGetParam", "(JI)J", (void *)nativeGetParam},
    {"nativeSetDisplaySurface", "(JLjava/lang/Object;)V",
     (void *)nativeSetDisplaySurface},
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  DO_USE_VAR(reserved);

  JNIEnv *env = NULL;
  if (vm->GetEnv((void **)&env, JNI_VERSION_1_4) != JNI_OK || !env) {
    __android_log_print(ANDROID_LOG_ERROR, "ddgplayer_jni",
                        "ERROR: failed to get jni env\n");
    return -1;
  }
  jclass cls = env->FindClass("com/github/ddgrcf/ddgplayer/MediaPlayer");
  int ret = env->RegisterNatives(cls, g_methods,
                                 sizeof(g_methods) / sizeof(g_methods[0]));
  if (ret != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, "ddgplayer_jni",
                        "ERROR: failed to register native methods\n");
    return -1;
  }

  g_jvm = vm;
  av_jni_set_java_vm(vm, NULL);
  return JNI_VERSION_1_4;
}

JNIEXPORT JavaVM *get_jni_jvm(void) {
  return g_jvm;
}

JNIEXPORT JNIEnv *get_jni_env(void) {
  JNIEnv *env;
  int status;
  if (g_jvm == NULL) {
    __android_log_print(ANDROID_LOG_ERROR, "ddgplayer_jni",
                        "ERROR: g_jvm is null !\n");
    return NULL;
  }

  status = g_jvm->GetEnv((void **)&env, JNI_VERSION_1_4);
  if (status != JNI_OK) {
    status = g_jvm->AttachCurrentThread(&env, NULL);
    if (status != JNI_OK) {
      __android_log_print(ANDROID_LOG_ERROR, "ddgplayer_jni",
                          "ERROR: failed to attach current thread !\n");
      return NULL;
    }
  }
  return env;
}

void JniAttachCurrentThread(void) {
  get_jni_env();
}

void JniDetachCurrentThread(void) {
  g_jvm->DetachCurrentThread();
}

void *JniRequestWinObj(void *data) {
  return data ? get_jni_env()->NewGlobalRef((jobject)data) : NULL;
}

void JniReleaseWinObj(void *data) {
  if (data) {
    get_jni_env()->DeleteGlobalRef((jobject)data);
  }
}

void JniPostMessage(void *extra, int32_t msg, void *param) {
  JNIEnv *env = get_jni_env();
  jobject obj = (jobject)extra;
  jmethodID mid = env->GetMethodID(env->GetObjectClass(obj),
                                   "internalPlayerEventCallback", "(IJ)V");
  env->CallVoidMethod(obj, mid, msg, (unsigned long)param);
}
