#include "ffrender.h"

#include <limits.h>

#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "stdefine.h"

#ifdef ANDROID
#include "fanplayer_jni.h"
#endif

typedef struct {
  uint8_t* adev_buf_data;
  uint8_t* adev_buf_cur;

  int adev_buf_size;
  int adev_buf_avail;

  void* surface; // 生产者和消费者的交换区
  AVRational frmrate;

  CommonVars* cmnvars;

  void* adev;
  void* vdev;

  // resample and scaler
  struct SwrContext* swr_context;
  struct SwrContext* sws_context;

  int cur_speed_type;
  int cur_speed_value;
  int new_speed_type;
  int new_speed_value;

  int swr_src_format;
  int swr_src_samprate;
  int swr_src_chlayout;

  int sws_src_pixfmt;
  int sws_src_width;
  int sws_src_height;
  int sws_dst_pixfmt;
  int sws_dst_width;
  int sws_dst_height;

  int cur_video_w;
  int cur_video_h;
  Rect cur_src_rect;
  Rect new_src_rect;

#define SW_VOLUME_MINDB -30 // 最小分贝数
#define SW_VOLUME_MAXDB +12 // 最大分贝数
  int vol_scalar[256]; // scale的映射矩阵
  int vol_zerodb;
  int vol_curval;

#define RENDER_CLOSE              (1 << 0)
#define RENDER_PAUSE              (1 << 1)
#define RENDER_SNAPSHOT           (1 << 2)
#define RENDER_STEPFORWARD        (1 << 3)
#define RENDER_DEFINITION_EVAL    (1 << 4)
  int status;
  float definitionval;

#if CONFIG_ENALBE_VEFFECT
  void* veffect_context;
  int veffect_type;
  int veffect_x;
  int veffect_y;
  int veffect_w;
  int veffect_h;
#endif

#if CONFIG_ENABLE_SNAPSHOT
  char snapfile[PATH_MAX];
  int snapwidth;
  int snapheight;
#endif

} Render;


// 就是将mindb和maxdb进行缩放
static int swvol_scalar_init(int* scalar, int mindb, int maxdb) {
  double tabdb[256];
  double tabf[256];
  int z, i;

  for (i = 0; i < 256; i++) {
    tabdb[i] = mindb + (double)(maxdb - mindb) * i / 256;
    tabf[i] = pow(10.0, tabdb[i] / 20.0);
    scalar[i] = (int)((1 << 14) * tabf[i]);
  }

  z = -mindb * 256 / (maxdb - mindb);
  z = MAX(z, 0);
  z = MIN(z, 255);
  scalar[0] = 0;
  scalar[z] = (1 << 14);

  return z;
}

// 将buf里面所有的元素使用定点数乘法
static void swvol_scalar_run(int16_t* buf, int n, int multiplier) {
  if (multiplier > (1 << 14)) {
    int32_t v;
    while (n--) {
      v = ((int32_t)*buf * multiplier) >> 14;
      v = MAX(v, -0x7fff);
      v = MIN(v, 0x7fff);
      *buf = (int16_t)v;
      buf++;
    }
  } else if (multiplier < (1 << 14)) {
    while (n--) { *buf = ((int32_t)*buf * multiplier) >> 14; buf++; }
  }
}

// static void render_setspeed(Render* render, int speed) {
// if (speed <= 0) { return; }
// }

void* render_open(int adevtype, int vdevtype, void* surface, struct AVRational frate, int w, int h, CommonVars* cmnvars) {
  Render* render = (Render*)calloc(1, sizeof(Render));
  if (!render) { return NULL; }

  DO_USE_VAR(surface);
  render->frmrate = frate;
  render->cmnvars = cmnvars;
  // render->adev_buf_avail = render->adev_buf_size = (int)((double))
  return NULL;
}
