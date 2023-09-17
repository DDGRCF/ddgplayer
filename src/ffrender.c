#include "ffrender.h"

#include <limits.h>

#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "adev.h"
#include "stdefine.h"
#include "vdev.h"
#include "veffect.h"

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
  struct SwrContext* swr_context; // 音频的格式变化
  struct SwsContext* sws_context; // 视频的格式变化

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
  int vol_scalar[256];      // scale的映射矩阵
  int vol_zerodb;
  int vol_curval;

#define RENDER_CLOSE           (1 << 0)
#define RENDER_PAUSE           (1 << 1)
#define RENDER_SNAPSHOT        (1 << 2)
#define RENDER_STEPFORWARD     (1 << 3)
#define RENDER_DEFINITION_EVAL (1 << 4)
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
    while (n--) {
      *buf = ((int32_t)*buf * multiplier) >> 14;
      buf++;
    }
  }
}

static void render_setspeed(Render* render, int speed) {
  if (speed <= 0) {
    return;
  }
  vdev_setparam(render->vdev, PARAM_PLAY_SPEED_VALUE, &speed);
  render->new_speed_value = speed; // 这里在渲染器中需要重复对比
}

static int render_audio_swresample(Render* render, AVFrame* audio) {
  return 0;
}

void* render_open(int adevtype, int vdevtype, void* surface,
                  struct AVRational frate, int w, int h, CommonVars* cmnvars) {
  Render* render = (Render*)calloc(1, sizeof(Render));
  if (!render) {
    return NULL;
  }

  DO_USE_VAR(surface);
  render->frmrate = frate;
  render->cmnvars = cmnvars;

  render->adev_buf_avail = render->adev_buf_size =
      (int)((double)ADEV_SAMPLE_RATE / (h ? 60 : 46) + 0.5) *
      4; // 为什么要这么做
  render->adev_buf_cur = render->adev_buf_data = malloc(render->adev_buf_size);

  render->adev = adev_create(adevtype, 5, render->adev_buf_size, cmnvars);
  render->vdev = vdev_create(vdevtype, surface, 0, w, h,
                             FF_TIME_MS * frate.den / frate.num, cmnvars);

#if CONFIG_ENABLE_SOUNDTOUCH
  render->stcontext = soundtouch_createInstance();
  soundtouch_setSampleRate(render->stcontext, ADEV_SAMPLE_RATE);
  soundtouch_setChannels(render->stcontext, 2);
#endif

#if CONFIG_ENALBE_VEFFECT
  render->veffect_context = veffect_create(surface);
#endif

  render_setspeed(render, 100);

  render->vol_zerodb =
      swvol_scalar_init(render->vol_scalar, SW_VOLUME_MINDB, SW_VOLUME_MAXDB);
  render->vol_curval = render->vol_zerodb;

  if (render->cmnvars->init_params->swscale_type == 0) {
    render->cmnvars->init_params->swscale_type = SWS_FAST_BILINEAR;
  } // 快速双线性插值

  return render;
}

void render_close(void* hrender) {
  Render* render = (Render*)hrender;
  if (!render) {
    return;
  }

  render->status = RENDER_CLOSE;

  adev_destroy(render->adev);

  swr_free(&render->swr_context);

  vdev_destroy(render->vdev);

  if (render->sws_context) {
    sws_freeContext(render->sws_context);
  }

#if CONFIG_ENALBE_VEFFECT
  veffect_destroy(render->veffect_context);
#endif

#if CONFIG_ENABLE_SOUNDTOUCH
  veffect_destroy(render->stcontext);
#endif

#ifdef ANDROID
  JniReleaseWinObj(render->surface);
#endif

  free(render->adev_buf_data);
  free(render);
}

void render_audio(void* hrender, AVFrame* audio) {
  Render* render = (Render*)hrender;
  int samprate, sampnum;
  if (!render ||
      (render->cmnvars->init_params->avts_syncmode != AVSYNC_MODE_FILE &&
       render->cmnvars->apktn > render->cmnvars->init_params->audio_bufpktn)) {
    return;
  } // TODO: 怎么audio_apktn 和 audio_bufpktn 的区别
  do {
    if (render->swr_src_format != audio->format ||
        render->swr_src_samprate != audio->sample_rate ||
        render->swr_src_chlayout != audio->channel_layout ||
        render->cur_speed_type != render->new_speed_type ||
        render->cur_speed_value !=
            render->new_speed_value) { // TODO: swr_src_chlayout 取代
      render->swr_src_format = (int)audio->format;
      render->swr_src_samprate = (int)audio->sample_rate;
      render->swr_src_chlayout = (int)audio->channel_layout;
      render->cur_speed_type = render->new_speed_type;
      render->cur_speed_value = render->new_speed_value;
      samprate =
          render->cur_speed_type
              ? ADEV_SAMPLE_RATE
              : (int)(ADEV_SAMPLE_RATE * 100.0 / render->cur_speed_value);
      if (render->swr_context) {
        swr_free(&render->swr_context);
      }
      render->swr_context = swr_alloc_set_opts(
          NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, samprate,
          render->swr_src_chlayout, render->swr_src_format,
          render->swr_src_samprate, 0, NULL);
      swr_init(render->swr_context);
#if CONFIG_ENABLE_SOUNDTOUCH
      if (render->cur_speed_type) {
        soundtouch_setTempo(render->stcontext,
                            (float)render->cur_speed_value / 100);
      }
#endif
    }

#if CONFIG_ENABLE_SOUNDTOUCH
    if (render->cur_speed_type && render->cur_speed_value != 100) {
      sampnum = render_audio_soundtouch(render, audio);
    } else
#endif
    {
      sampnum = render_audio_swresample(render, audio);
    }
    while ((render->status & RENDER_PAUSE & RENDER_CLOSE)) {
      av_usleep(10 * FF_TIME_MS);
    }
  } while (sampnum && !(render->status & RENDER_CLOSE));
}

void render_video(void* hrender, AVFrame* video) {}

void render_setrect(void* hrender, int type, int x, int y, int w, int h) {}

void render_pause(void* hrender, int pause) {
  Render* render = (Render*)hrender;
  if (!render) {
    return;
  }

  switch (pause) {
    case 0:
      render->status &= ~RENDER_PAUSE;
      break; // 取消暂停
    case 1:
      render->status |= RENDER_PAUSE;
      break; // 开始暂停
    case 2:
      render->status = RENDER_CLOSE;
      break; // 关闭渲染器
  }

  // 每次暂停前需要记录时间，要不然无法确认时间
  render->cmnvars->start_tick =
      av_rescale_q(av_gettime_relative(), AV_TIME_BASE_Q, FF_TIME_BASE_Q);
  render->cmnvars->start_pts =
      MAX(render->cmnvars->apts, render->cmnvars->vpts);
}

int render_snapshot(void* hrender, char* file, int w, int h, int wait_time) {
#if CONFIG_ENABLE_SNAPSHOT
  Render* render = (Render*)hrender;
  if (!hrender) {
    return -1;
  }

  if (render->status & RENDER_SNAPSHOT) {
    return -1;
  }

  strcpy(render->snapfile, file);
  render->snapwidth = w;
  render->snapheight = h;

  render->status |= RENDER_SNAPSHOT;

  if (wait_time > 0) {
    int retry = wait_time / 10;
    while ((render->status & RENDER_SNAPSHOT) && retry--) {
      av_usleep(10 * FF_TIME_MS);
    }
  }
#else
  DO_USE_VAR(hrender);
  DO_USE_VAR(file);
  DO_USE_VAR(w);
  DO_USE_VAR(h);
  DO_USE_VAR(wait_time);
#endif
  return 0;
}

void render_setparam(void* hrender, int id, void* param) {}

void render_getparam(void* hrender, int id, void* param) {}
