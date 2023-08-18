#include "vdev.h"

#include <pthread.h>

#include <libavutil/log.h>
#include <libavutil/time.h>

#include "ffplayer.h"


static void vdev_setup_vrect(VdevCommonContext* vdev) {
  int rw = vdev->rrect.right - vdev->rrect.left, rh = vdev->rrect.bottom - vdev->rrect.top, vw, vh;

  // 下面这样做能保证矩形始终在视频的可视化界面内
  if (vdev->vm == VIDEO_MODE_LETTERBOX) {
    if (rw * vdev->vh < rh * vdev->vw) { // 比实际更加宽一点
      vw = rw; vh = vw * vdev->vh / vdev->vw;
    } else { // 比实际更加长一点
      vh = rh; vw = vh * vdev->vw / vdev->vh;
    }
  } else {
    vw = rw; vh = rh;
  }

  vdev->vrect.left = (rw - vw) / 2;
  vdev->vrect.top = (rh - vh) / 2;
  vdev->vrect.right = vdev->vrect.left + vw;
  vdev->vrect.bottom = vdev->vrect.top + vh;
  vdev->status |= VDEV_CLEAR;
}


void* vdev_create(int type, void* surface, int bufnum, int w, int h, int ftime, CommonVars* cmnvars) {
  VdevCommonContext* context = NULL; 

#ifdef ANDROID
  context = (VdevCommonContext*)vdev_android_create(surface, bufnum);
  if (!context) { return NULL; }
  context->tickavdiff =- ftime * 2; // TODO: ?
#endif
  context->vw = MAX(w, 1);
  context->vh = MAX(h, 1);
  context->rrect.right = MAX(w, 1);
  context->rrect.bottom = MAX(h, 1);
  context->vrect.right = MAX(w, 1);
  context->vrect.bottom = MAX(h, 1);
  context->speed = 100; // TODO:
  context->tickframe = ftime;
  context->ticksleep = ftime;
  context->cmnvars = cmnvars;
  return context;
}

void vdev_destroy(void* ctxt) {
  VdevCommonContext* context = (VdevCommonContext*)ctxt;

  if (context->thread) {
    pthread_mutex_lock(&context->mutex);
    context->status = VDEV_CLOSE;
    pthread_cond_signal(&context->cond);
    pthread_mutex_unlock(&context->mutex);
    pthread_join(context->thread, NULL);
  }

  if (context->destroy) { context->destroy(context); }
}

void vdev_unlock(void* ctxt) {
  VdevCommonContext* context = (VdevCommonContext*)ctxt;
  if (context->unlock) { context->unlock(context); }
}

void vdev_setrect(void* ctxt, int x, int y, int w, int h) {
  VdevCommonContext* context = (VdevCommonContext*)ctxt;
  w = w > 1 ? w : 1;
  h = h > 1 ? h : 1;
  pthread_mutex_lock(&context->mutex);
  context->rrect.left = x;
  context->rrect.top = y;
  context->rrect.right = x + w;
  context->rrect.bottom = y + h;
  vdev_setup_vrect(context);
  pthread_mutex_unlock(&context->mutex);
  if (context->setrect) { context->setrect(context, x, y, w, h); }
}

void vdev_setparam(void* ctxt, int id, void* param) {
  VdevCommonContext* context = (VdevCommonContext*)ctxt;
  if (!context) { return; }
  switch (id) {
    case PARAM_VIDEO_MODE:
      pthread_mutex_lock(&context->mutex);
      context->vw = *(int*)param;
      vdev_setup_vrect(context);
      pthread_mutex_unlock(&context->mutex);
      break;
    case PARAM_PLAY_SPEED_VALUE:
      if (param) { context->tickavdiff = *(int*)param; }
      break;
    case PARAM_AVSYNC_TIME_DIFF:
      if (param) { context->tickavdiff = *(int*)param; }
      break;
    case PARAM_VDEV_SET_BBOX:
      context->bbox_list = param;
  }

  if (context->setparam) { context->setparam(context, id, param); }
}

void vdev_getparam(void* ctxt, int id, void* param) {
  VdevCommonContext* context = (VdevCommonContext*)ctxt;
  if (!ctxt || !param) { return; }
  switch (id) {
    case PARAM_VIDEO_MODE : *(int*)param = context->vm; break;
    case PARAM_PLAY_SPEED_VALUE : *(int*)param = context->speed; break;
    case PARAM_AVSYNC_TIME_DIFF : *(int*)param = context->tickavdiff; break;
    case PARAM_VDEV_GET_VRECT : *(Rect*)param = context->vrect; break;
  }
  if (context->getparam) { context->getparam(context, id, param); }
}


void vdev_avsync_and_complete(void* ctxt) {

}
