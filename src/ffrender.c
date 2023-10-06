#include "ffrender.h"

#include <limits.h>

#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "adev.h"
#include "ffplayer.h"
#include "stdefine.h"
#include "vdev.h"
#include "veffect.h"

#ifdef ANDROID

#include "ddgplayer_jni.h"

#endif

typedef struct {
  uint8_t *adev_buf_data;
  uint8_t *adev_buf_cur;

  int adev_buf_size;
  int adev_buf_avail;

  void *surface; // 生产者和消费者的交换区
  AVRational frmrate;

  CommonVars *cmnvars;

  void *adev;
  void *vdev;

  // resample and scaler
  struct SwrContext *swr_context; // 音频的格式变化
  struct SwsContext *sws_context; // 视频的格式变化

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

#if CONFIG_ENABLE_SOUNDTOUCH
  void *stcontext;
#endif

#if CONFIG_ENALBE_VEFFECT
  void *veffect_context;
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
static int swvol_scalar_init(int *scalar, int mindb, int maxdb) {
  double tabdb[256];
  double tabf[256];
  int z, i;
  for (i = 0; i < 256; i++) {
    tabdb[i] = mindb + (double)(maxdb - mindb) * i / 256;
    tabf[i] = pow(10.0, tabdb[i] / 20.0);
    scalar[i] = (int)((1 << 14) * tabf[i]);
  }

  // 这里maxdb为正的，mindb为负的
  z = -mindb * 256 / (maxdb - mindb); // zero: -30 ....-> [0] <- .. +12
  z = MAX(z, 0);
  z = MIN(z, 255);
  scalar[0] = 0;
  scalar[z] = (1 << 14);

  return z;
}

/**
 * @brief 将buf里面的所有的元素使用定点数乘法进行计算
 */
static void swvol_scalar_run(int16_t *buf, int n, int multiplier) {
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

static void render_setspeed(Render *render, int speed) {
  if (speed <= 0) {
    return;
  }
  vdev_setparam(render->vdev, PARAM_PLAY_SPEED_VALUE, &speed);
  render->new_speed_value = speed; // 这里在渲染器中需要重复对比
}

static int render_audio_swresample(Render *render, AVFrame *audio) {
  int num_sample;

  // out是输出的buf，而out_n是输出的样本数，输入的样本数据，单通道数量
  num_sample =
      swr_convert(render->swr_context, (uint8_t **)&render->adev_buf_cur,
                  render->adev_buf_avail / 4,
                  (const uint8_t **)audio->extended_data, audio->nb_samples);
  audio->extended_data = NULL;
  audio->nb_samples = 0;
  render->adev_buf_avail -= num_sample * 4;
  render->adev_buf_cur += num_sample * 4;

  if (render->adev_buf_avail == 0) {
#if CONFIG_ENABLE_VEFFECT
    if (render->veffect_type != VISUAL_EFFECT_DISABLE) {
      veffect_render(render->veffect_context, render->veffect_x,
                     render->veffect_y, render->veffect_w, render->veffect_h,
                     render->veffect_type, render->adev);
    }
#endif
    swvol_scalar_run((int16_t *)render->adev_buf_data,
                     render->adev_buf_size / sizeof(int16_t),
                     render->vol_scalar[render->vol_curval]);
    audio->pts +=
        5 * render->cur_speed_value * render->adev_buf_size /
        (2 * ADEV_SAMPLE_RATE); // 播放前把时间戳计算好 TODO(ddgrcf): 计算方式
    adev_write(render->adev, render->adev_buf_data, render->adev_buf_size,
               audio->pts);
    render->adev_buf_avail = render->adev_buf_size;
    render->adev_buf_cur = render->adev_buf_data;
  }
  return num_sample;
}

/**
  * @brief 锐度评估函数
  */
static float definition_evaluation(uint8_t *img, int w, int h, int stride) {
  uint8_t *cur, *pre, *nxt;
  int i, j, l;
  int64_t s = 0;

  if (!img || !w || !h || !stride)
    return 0;
  pre = img + 1;
  cur = img + 1 + stride * 1;
  nxt = img + 1 + stride * 2;

  for (i = 1; i < h - 1; i++) {
    for (j = 1; j < w - 1; j++) {
      l = 1 * pre[-1] + 4 * pre[0] + 1 * pre[1];
      l += 4 * cur[-1] - 20 * cur[0] + 4 * cur[1];
      l += 1 * nxt[-1] + 4 * nxt[0] + 1 * nxt[1];
      s += abs(l);
      pre++;
      cur++;
      nxt++;
    }
    pre += stride - (w - 2);
    cur += stride - (w - 2);
    nxt += stride - (w - 2);
  }
  return (float)s / ((w - 2) * (h - 2));
}

static void render_setup_srcrect(Render *render, AVFrame *video,
                                 AVFrame *srcpic) {
  srcpic->pts = video->pts;
  srcpic->format = video->format;
  srcpic->width = render->cur_src_rect.right - render->cur_src_rect.left;
  srcpic->height = render->cur_src_rect.bottom - render->cur_src_rect.top;
  memcpy(srcpic->data, video->data, sizeof(srcpic->data));
  memcpy(srcpic->linesize, video->linesize, sizeof(srcpic->linesize));
  switch (video->format) {
    case AV_PIX_FMT_YUV420P:
      srcpic->data[0] += render->cur_src_rect.top * video->linesize[0] +
                         render->cur_src_rect.left;
      srcpic->data[1] += (render->cur_src_rect.top / 2) * video->linesize[1] +
                         (render->cur_src_rect.left / 2);
      srcpic->data[2] += (render->cur_src_rect.top / 2) * video->linesize[2] +
                         (render->cur_src_rect.left / 2);
      break;
    case AV_PIX_FMT_NV21:
    case AV_PIX_FMT_NV12:
      srcpic->data[0] += render->cur_src_rect.top * video->linesize[0] +
                         render->cur_src_rect.left;
      srcpic->data[1] += (render->cur_src_rect.top / 2) * video->linesize[1] +
                         (render->cur_src_rect.left / 2) * 2;
      break;
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_BGRA:
      srcpic->data[0] += render->cur_src_rect.top * video->linesize[0] +
                         render->cur_src_rect.left * sizeof(uint32_t);
      break;
  }
}

void *render_open(int adevtype, int vdevtype, void *surface,
                  struct AVRational frate, int w, int h, CommonVars *cmnvars) {
  Render *render = (Render *)calloc(1, sizeof(Render));
  if (!render) {
    return NULL;
  }

  DO_USE_VAR(surface);
  render->frmrate = frate;
  render->cmnvars = cmnvars;

  render->adev_buf_avail = render->adev_buf_size =
      (int)((double)ADEV_SAMPLE_RATE / (h ? 60 : 46) + 0.5) *
      4; // TODO: * 4 是因为立体声和16bit，也就是/4是32bit
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
  } // swscale_type 设置的变化类型设置为 快速双线性插值

  return render;
}

void render_close(void *hrender) {
  Render *render = (Render *)hrender;
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

void render_audio(void *hrender, AVFrame *audio) {
  Render *render = (Render *)hrender;
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

void render_video(void *hrender, AVFrame *video) {

  Render *render = (Render *)hrender;
  if (!hrender)
    return;

  if (render->status & RENDER_DEFINITION_EVAL) { // TODO(ddgrcf):
    render->definitionval = definition_evaluation(
        video->data[0], video->width, video->height, video->linesize[0]);
    render->status &= ~RENDER_DEFINITION_EVAL;
  }

  // 但队列里面的帧的数量大于视频缓冲区帧的数量后，就不做任何操作
  if (render->cmnvars->init_params->avts_syncmode != AVSYNC_MODE_FILE &&
      render->cmnvars->vpktn > render->cmnvars->init_params->video_bufpktn)
    return;
  do {
    VdevCommonContext *vdev = (VdevCommonContext *)render->vdev;
    AVFrame lockedpic = *video, srcpic, dstpic = {{0}};
    if (render->cur_video_w != video->width ||
        render->cur_video_h != video->height) {
      render->cur_video_w = render->new_src_rect.right = video->width;
      render->cur_video_h = render->new_src_rect.bottom = video->height;
    }
    if (memcmp(&render->cur_src_rect, &render->new_src_rect, sizeof(Rect)) !=
        0) { // 设置展示的矩形框
      render->cur_src_rect.left = MIN(render->new_src_rect.left, video->width);
      render->cur_src_rect.top = MIN(render->new_src_rect.top, video->height);
      render->cur_src_rect.right =
          MIN(render->new_src_rect.right, video->width);
      render->cur_src_rect.bottom =
          MIN(render->new_src_rect.bottom, video->height);
      render->new_src_rect = render->cur_src_rect;
      vdev->vw = MAX(render->cur_src_rect.right - render->cur_src_rect.left, 1);
      vdev->vh = MAX(render->cur_src_rect.bottom - render->cur_src_rect.top, 1);
      vdev_setparam(vdev, PARAM_VIDEO_MODE, &vdev->vm);
    }

    render_setup_srcrect(render, &lockedpic,
                         &srcpic); // 将lockedpic中的数据拷贝到srcpic中
    vdev_lock(render->vdev, dstpic.data, dstpic.linesize,
              srcpic.pts); // 设备加锁，防止其他线程写入，让设备被一个线程独占
    if (dstpic.data[0] && srcpic.format != -1 && srcpic.pts != -1) {
      if (render->sws_src_pixfmt != srcpic.format ||
          render->sws_src_width != srcpic.width ||
          render->sws_src_height != srcpic.height ||
          render->sws_dst_pixfmt != vdev->pixfmt ||
          render->sws_dst_width != dstpic.linesize[6] ||
          render->sws_dst_height != dstpic.linesize[7]) {
        render->sws_src_pixfmt = srcpic.format;
        render->sws_src_width = srcpic.width;
        render->sws_src_height = srcpic.height;
        render->sws_dst_pixfmt = vdev->pixfmt;
        render->sws_dst_width = dstpic.linesize[6];
        render->sws_dst_height = dstpic.linesize[7];
        if (render->sws_context)
          sws_freeContext(render->sws_context);
        render->sws_context =
            sws_getContext(render->sws_src_width, render->sws_src_height,
                           render->sws_src_pixfmt, render->sws_dst_width,
                           render->sws_dst_height, render->sws_dst_pixfmt,
                           render->cmnvars->init_params->swscale_type, 0, 0,
                           0); // 如果尺寸不对，就开始变化
      }
      if (render->sws_context)
        sws_scale(render->sws_context, (const uint8_t **)srcpic.data,
                  srcpic.linesize, 0, render->sws_src_height, dstpic.data,
                  dstpic.linesize); // 变化后的数据
    }
    vdev_unlock(render->vdev); // 设备解锁，让其他线程又可以写入

#if CONFIG_ENABLE_SNAPSHOT
    // if (render->status & RENDER_SNAPSHOT) {
    // take_snapshot(render->snapfile, render->snapwidth, render->snapheight, &lockedpic);
    // player_send_message(render->cmnvars->winmsg, MSG_TAKE_SNAPSHOT, 0);
    // render->status &= ~RENDER_SNAPSHOT;
    // }
#endif
  } while ((render->status & RENDER_PAUSE) &&
           !(render->status & RENDER_STEPFORWARD)); // 快进

  // clear step forward flag
  render->status &= ~RENDER_STEPFORWARD; // TODO
}

void render_setrect(void *hrender, int type, int x, int y, int w, int h) {
  Render *render = (Render *)hrender;
  if (!hrender)
    return;
  switch (type) {
    case 0:
      vdev_setrect(render->vdev, x, y, w, h);
      break;
#if CONFIG_ENABLE_VEFFECT
    case 1:
      render->veffect_x = x;
      render->veffect_y = y;
      render->veffect_w = MAX(w, 1);
      render->veffect_h = MAX(h, 1);
      break;
#endif
  }
}

void render_pause(void *hrender, int pause) {
  Render *render = (Render *)hrender;
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

int render_snapshot(void *hrender, char *file, int w, int h, int wait_time) {
#if CONFIG_ENABLE_SNAPSHOT
  Render *render = (Render *)hrender;
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

void render_setparam(void *hrender, int id, void *param) {
  Render *render = (Render *)hrender;
  if (!hrender) {
    return;
  }
  switch (id) {
    case PARAM_AUDIO_VOLUME: {
      int vol = *(int *)param;
      vol += render->vol_zerodb;
      vol = MAX(vol, 0);
      vol = MIN(vol, 255);
      render->vol_curval = vol;
    }
    case PARAM_PLAY_SPEED_VALUE:
      render_setspeed(render, *(int *)param);
      break;
    case PARAM_PLAY_SPEED_TYPE:
      render->new_speed_type = *(int *)param;
      break;
#if CONFIG_ENABLE_VEFFECT
    case PARAM_VISUAL_EFFECT:
      render->veffect_type = *(int *)param;
      if (render->veffet_type == VISUAL_EFFECT_DISABLE) {
        veffect_render(render->veffect_context, render->veffect_x,
                       render->veffect_y, render->veffect_w, render->veffect_h,
                       render->veffet_type, render->adev);
      }
#endif
    case PARAM_VIDEO_MODE:
    case PARAM_AVSYNC_TIME_DIFF:
    case PARAM_VDEV_POST_SURFACE:
    case PARAM_VDEV_SET_OVERLAY_RECT:
      vdev_setparam(render->vdev, id, param);
      break;
    case PARAM_RENDER_STEPFORWARD:
      render->status |= RENDER_STEPFORWARD;
      break;
    case PARAM_RENDER_VDEV_WIN:
#ifdef ANDROID
      JniReleaseWinObj(render->surface);
      render->surface = JniRequestWinObj(param);
      vdev_setparam(render->vdev, id, render->surface);
#endif
      break;
    case PARAM_RENDER_SOURCE_RECT:
      if (param) {
        render->new_src_rect = *(Rect *)param;
      }
      if (render->new_src_rect.right == 0 && render->new_src_rect.bottom == 0) {
        render->cur_video_w = render->cur_video_h = 0;
      }
      break;
    default:
      break;
  }
}

void render_getparam(void *hrender, int id, void *param) {
  Render *render = (Render *)hrender;
  VdevCommonContext *vdev = render ? (VdevCommonContext *)render->vdev : NULL;
  if (!hrender) {
    return;
  }
  switch (id) {
    case PARAM_MEDIA_POSITION:
      if (vdev && vdev->status & VDEV_COMPLETED) {
        *(int64_t *)param = -1;
      } else {
        *(int64_t *)param = render->cmnvars->apts != -1 ? render->cmnvars->apts
                                                        : render->cmnvars->vpts;
      }
      break;
    case PARAM_AUDIO_VOLUME:
      *(int *)param = render->vol_curval - render->vol_zerodb;
      break;
    case PARAM_PLAY_SPEED_VALUE:
      *(int *)param = render->cur_speed_value;
      break;
    case PARAM_PLAY_SPEED_TYPE:
      *(int *)param = render->cur_speed_type;
      break;
#if CONFIG_ENALBE_VEFFECT
    case PARAM_VISUAL_EFFECT： *(int *)param = render->veffect_type; break;
#endif
        case PARAM_VIDEO_MODE:
    case PARAM_AVSYNC_TIME_DIFF:
    case PARAM_VDEV_GET_OVERLAY_HDC:
    case PARAM_VDEV_GET_VRECT:
      vdev_getparam(vdev, id, param);
      return;
    case PARAM_ADEV_GET_CONTEXT:
      *(void **)param = render->adev;
      break;
    case PARAM_VDEV_GET_CONTEXT:
      *(void **)param = render->adev;
      break;
    case PARAM_DEFINITION_VALUE:
      *(float *)param = render->definitionval;
      render->status |= RENDER_DEFINITION_EVAL;
      break;
    case PARAM_RENDER_SOURCE_RECT:
      *(Rect *)param = render->cur_src_rect;
      break;
    default:
      break;
  }
}
