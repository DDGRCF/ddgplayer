#include "adev.h"

#include <jni.h>
#include <pthread.h>

#include "stdefine.h"

typedef struct {
  int16_t *data;
  int32_t size;
} AudioBuf;

#define DEF_ADEV_BUF_NUM 3
#define DEF_ADEV_BUF_LEN 2048

JNIEXPORT JavaVM *get_jni_jvm(void);
JNIEXPORT JNIEnv *get_jni_env(void);

typedef struct {
  ADEV_COMMON_MEMBERS;

  uint8_t *p_wave_buf;  // 指向数据
  AudioBuf *p_wave_hdr; // 指向数据(多了一个长度)
  int curnum;

  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;

  jobject jobj_at;
  jmethodID jmid_at_init;
  jmethodID jmid_at_close;
  jmethodID jmid_at_play;
  jmethodID jmid_at_pause;
  jmethodID jmid_at_write;
  jbyteArray audio_buffer;

} AdevContext;

static void *audio_render_thread_proc(void *param) {
  JNIEnv *env = get_jni_env();
  AdevContext *context = (AdevContext *)param;

  env->CallVoidMethod(context->jobj_at, context->jmid_at_play);

  while (!(context->status & ADEV_CLOSE)) {
    pthread_mutex_lock(&context->lock);
    while (context->curnum == 0 && !(context->status & ADEV_CLOSE)) {
      pthread_cond_wait(&context->cond, &context->lock);
    }

    if (!(context->status & ADEV_CLOSE)) {
      env->CallIntMethod(context->jobj_at, context->jmid_at_write,
                         context->audio_buffer, context->head * context->buflen,
                         context->p_wave_hdr[context->head].size); // TODO:
      context->cmnvars->apts = context->ppts[context->head];
      if (++context->head == context->bufnum) {
        context->head = 0;
      }
      pthread_cond_signal(&context->cond);
    }
    pthread_mutex_unlock(&context->lock);
  }

  env->CallVoidMethod(context->jobj_at, context->jmid_at_close);

  get_jni_jvm()->DetachCurrentThread();
  return NULL;
}

void *adev_create(int type, int bufnum, int buflen, CommonVars *cmnvars) {
  JNIEnv *env = get_jni_env();
  AdevContext *context = NULL;
  int i;

  DO_USE_VAR(type);
  bufnum = bufnum ? bufnum : DEF_ADEV_BUF_NUM;
  buflen = buflen ? buflen : DEF_ADEV_BUF_LEN;

  context =
      (AdevContext *)calloc(1, sizeof(AdevContext) + bufnum * sizeof(int64_t) +
                                   bufnum * sizeof(AudioBuf));
  if (!context) {
    return NULL;
  }
  context->bufnum = bufnum;
  context->buflen = buflen;
  context->ppts = (int64_t *)((uint8_t *)context + sizeof(AdevContext));
  context->p_wave_hdr =
      (AudioBuf *)((uint8_t *)context->ppts + bufnum * sizeof(int64_t));
  context->cmnvars = cmnvars;

  jbyteArray local_audio_buffer = env->NewByteArray(bufnum * buflen);
  context->audio_buffer = (jbyteArray)env->NewGlobalRef(local_audio_buffer);
  context->p_wave_buf =
      (uint8_t *)env->GetByteArrayElements(context->audio_buffer, 0);
  env->DeleteLocalRef(local_audio_buffer); // 也就是将生命周期权权交给jvm

  for (i = 0; i < bufnum; i++) {
    context->p_wave_hdr[i].data = (int16_t *)(context->p_wave_buf + i * buflen);
    context->p_wave_hdr[i].size = buflen;
  }

  jclass jcls = env->FindClass("android/media/AudioTrack");
  context->jmid_at_init = env->GetMethodID(jcls, "<init>", "(IIIIII)V");
  context->jmid_at_close = env->GetMethodID(jcls, "release", "()V");
  context->jmid_at_play = env->GetMethodID(jcls, "play", "()V");
  context->jmid_at_pause = env->GetMethodID(jcls, "pause", "()V");
  context->jmid_at_write = env->GetMethodID(jcls, "write", "([BII)I");

#define STREAM_MUSIC       3
#define ENCODING_PCM_16BIT 2
#define CHANNEL_STEREO     3
#define MODE_STREAM        1

  jobject at_obj = env->NewObject(
      jcls, context->jmid_at_init, STREAM_MUSIC, ADEV_SAMPLE_RATE,
      CHANNEL_STEREO, ENCODING_PCM_16BIT, context->buflen * 2, MODE_STREAM);
  context->jobj_at = env->NewGlobalRef(at_obj);
  env->DeleteLocalRef(at_obj);

  pthread_mutex_init(&context->lock, NULL);
  pthread_cond_init(&context->cond, NULL);

  pthread_create(&context->thread, NULL, audio_render_thread_proc, context);

  return context;
}

void adev_destroy(void *ctxt) {
  if (!ctxt)
    return;
  JNIEnv *env = get_jni_env();
  AdevContext *context = (AdevContext *)ctxt;

  pthread_mutex_lock(&context->lock);
  context->status |= ADEV_CLOSE;
  pthread_cond_signal(&context->cond);
  pthread_mutex_unlock(&context->lock);
  pthread_join(context->thread, NULL);

  pthread_mutex_destroy(&context->lock);
  pthread_cond_destroy(&context->cond);

  env->ReleaseByteArrayElements(context->audio_buffer,
                                (jbyte *)context->p_wave_buf, 0);
  env->DeleteGlobalRef(context->audio_buffer);
  env->DeleteGlobalRef(context->jobj_at);

  free(context);
}

void adev_write(void *ctxt, uint8_t *buf, int len, int64_t pts) {
  if (!ctxt)
    return;
  AdevContext *context = (AdevContext *)ctxt;
  pthread_mutex_lock(&context->lock);
  while (context->curnum == context->bufnum &&
         (context->status & ADEV_CLOSE) == 0) {
    pthread_cond_wait(&context->cond, &context->lock);
  }

  if (context->curnum < context->bufnum) {
    memcpy(context->p_wave_hdr[context->tail].data, buf,
           MIN(context->p_wave_hdr[context->tail].size, len));
    context->curnum++;
    context->ppts[context->tail] = pts;
    if (++context->tail == context->bufnum) {
      context->tail = 0;
    }
    pthread_cond_signal(&context->cond);
  }
  pthread_mutex_unlock(&context->lock);
}

void adev_setparam(void *ctxt, int id, void *param) {}

void adev_getparam(void *ctxt, int id, void *param) {}
