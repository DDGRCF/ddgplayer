#ifndef DDGPLAYER_VDEV_H_
#define DDGPLAYER_VDEV_H_

#include <stdint.h>

#include "ffplayer.h"
#include "stdefine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VDEV_CLOSE     (1 << 0)
#define VDEV_COMPLETED (1 << 1)
#define VDEV_CLEAR     (1 << 2) // 清除数据

// rrect 渲染的矩形框 vrect 视频数据的矩形框
/*
 * rrect 视频渲染的矩形框(就是渲染后可能包括黑边)
 * vrect 视频数据的矩形框(真正渲染视频数据的范围)
 * tickframe 帧的时间，(如果为25FPS的话，1 / 25 * 1000 = 40，unit: ms)
 * ticksleep sleep的时间，(同上)
 */
#define VDEV_COMMON_MEMBERS                                                   \
  int bufnum;                                                                 \
  int pixfmt;                                                                 \
  int vw, vh, vm;                                                             \
  Rect rrect;                                                                 \
  Rect vrect;                                                                 \
                                                                              \
  void* surface;                                                              \
  int64_t* ppts;                                                              \
                                                                              \
  int head;                                                                   \
  int tail;                                                                   \
  int size;                                                                   \
                                                                              \
  pthread_mutex_t mutex;                                                      \
  pthread_cond_t cond;                                                        \
                                                                              \
  CommonVars* cmnvars;                                                        \
  int tickavdiff;                                                             \
  int tickframe;                                                              \
  int ticksleep;                                                              \
  int ticklast;                                                               \
  int speed;                                                                  \
  int status;                                                                 \
  pthread_t thread;                                                           \
                                                                              \
  int completed_counter;                                                      \
  int completed_apts;                                                         \
  int completed_vpts;                                                         \
  void* bbox_list;                                                            \
  void (*lock)(void* ctxt, uint8_t* buffer[8], int linesize[8], int64_t pts); \
  void (*unlock)(void* ctxt);                                                 \
  void (*setrect)(void* ctxt, int x, int y, int w, int h);                    \
  void (*setparam)(void* ctxt, int id, void* param);                          \
  void (*getparam)(void* ctxt, int id, void* param);                          \
  void (*destroy)(void* ctxt)

typedef struct {
  VDEV_COMMON_MEMBERS;
} VdevCommonContext;

#ifdef ANDROID
void* vdev_android_create(void* surface, int bufnum);
#endif

void* vdev_create(int type, void* surface, int bufnum, int w, int h, int ftime,
                  CommonVars* cmnvars);
void vdev_destroy(void* ctxt);
void vdev_setrect(void* ctxt, int x, int y, int w, int h);
void vdev_lock(void* ctxt, uint8_t* buffer[8], int linesize[8], int64_t pts);
void vdev_unlock(void* ctxt);
void vdev_setparam(void* ctxt, int id, void* param);
void vdev_getparam(void* ctxt, int id, void* param);

void vdev_avsync_and_complete(void* ctxt);

#ifdef __cplusplus
}
#endif

#endif
