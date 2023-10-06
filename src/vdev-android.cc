#include "vdev.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

extern "C" {
  #include "libavformat/avformat.h"
}

#include "stdefine.h"

JNIEXPORT JavaVM *get_jni_jvm(void);
JNIEXPORT JNIEnv *get_jni_env(void);

#define DEF_WIN_PIX_FMT WINDOW_FORMAT_RGBX_8888
#define VDEV_ANDROID_UPDATE_WIN (1 << 31)

typedef struct {
  VDEV_COMMON_MEMBERS;
  ANativeWindow *win;
} VdevContext;

inline int andorid_pixfmt_to_ffmpeg_pixfmt(int fmt) {
  switch (fmt) {
    case WINDOW_FORMAT_RGB_565: return AV_PIX_FMT_RGB565; break;
    case WINDOW_FORMAT_RGBX_8888: return AV_PIX_FMT_RGB32; break;
    default: return 0;
  } 
}

static void vdev_android_lock(void *ctxt, uint8_t *buffer[8], int linesize[8], int64_t pts) {
  VdevContext *context = (VdevContext *)ctxt;
  if (context->status & VDEV_ANDROID_UPDATE_WIN) {
    if (context->win) { ANativeWindow_release(context->win); context->win = NULL; }
    if (context->surface) { context->win = ANativeWindow_fromSurface(get_jni_env(), (jobject)context->surface); }
    if (context->win) { ANativeWindow_setBuffersGeometry(context->win, context->vm, context->vh, DEF_WIN_PIX_FMT); } 
  }
}

static void vdev_android_unlock(void *ctxt) {
  VdevContext *context = (VdevContext *)ctxt;
  if (context->win) { ANativeWindow_unlockAndPost(context->win); }
  vdev_avsync_and_complete(context);
}

static void vdev_android_destroy(void *ctxt) {
  VdevContext *context = (VdevContext *)ctxt;
  if (context->win) { ANativeWindow_release(context->win); }
  free(context);
}

static void vdev_android_setparam(void *ctxt, int id, void *param) {
  VdevContext *context = (VdevContext *)ctxt;
  switch (id) {
    case PARAM_RENDER_VDEV_WIN:
      context->surface = param;
      context->status |= VDEV_ANDROID_UPDATE_WIN;
      break;
  };
}

void *vdev_android_create(void *surface, int bufnum) {
  VdevContext *context = (VdevContext *)calloc(1, sizeof(VdevContext));
  if (!context) { return NULL; }
  context->pixfmt = andorid_pixfmt_to_ffmpeg_pixfmt(DEF_WIN_PIX_FMT);
  context->lock = vdev_android_lock;
  context->unlock = vdev_android_unlock;
  context->destroy = vdev_android_destroy;
  context->setparam = vdev_android_setparam;
  context->status |= VDEV_ANDROID_UPDATE_WIN;
  return context;
}
