#include "ffplayer.h"

#include <limits.h>
#include <pthread.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>

#include "adev.h"
#include "datarate.h"
#include "ffrender.h"
#include "pktqueue.h"
#include "recorder.h"
#include "stdefine.h"
#include "vdev.h"

#ifdef ANDROID
#include "ddgplayer_jni.h"
#endif

typedef struct {
  // muxer format
  AVFormatContext *avformat_context;

  // audio
  AVCodecContext *acodec_context;
  int32_t astream_index;
  AVRational astream_timebase;
  AVFrame aframe;

  // video
  AVCodecContext *vcodec_context; // 视频的上下文
  int32_t vstream_index;          // 视频的索引
  AVRational vstream_timebase;    // 视频的时间单位
  AVFrame vframe;                 // 提取到的视频帧，用于渲染
  AVRational vfrate;              // 视频的帧率

  // queue
  void *pktqueue; // 队列
  void *render;   // 渲染器
  void *datarate; // 码率

// thread status
#define PS_A_PAUSE   (1 << 0) // 音频暂停解码
#define PS_V_PAUSE   (1 << 1) // 视频暂停解码
#define PS_R_PAUSE   (1 << 2) // 渲染器暂停
#define PS_F_SEEK    (1 << 3)
#define PS_A_SEEK    (1 << 4)
#define PS_V_SEEK    (1 << 5)
#define PS_RECONNECT (1 << 6)
#define PS_CLOSE     (1 << 7)
  int status;

  // seek
  int seek_req;
  int64_t seek_pos;
  int64_t seek_dest;
  int64_t seek_vpts;

  int seek_diff;
  int seek_sidx; // TODO:

  // player configuration
  CommonVars cmnvars;

  pthread_mutex_t lock;
  pthread_t avdemux_thread;
  pthread_t adecode_thread;
  pthread_t vdecode_thread;

  AVFilterGraph *vfilter_graph;
  AVFilterContext *vfilter_src_ctx;
  AVFilterContext *vfilter_sink_ctx;

  // player init timeout, and init params
  int64_t read_timelast; // 上一次读取的时间，主要用于音视频同步(微秒)
  int64_t read_timeout;         // 读取是否超时
  PlayerInitParams init_params; // 播放器参数

  // path to play
  char url[PATH_MAX]; // 要播放的路径

  // recoder used for recording
  void *recorder;

} Player;

// 毫秒单位转化
const int FF_TIME_MS = 1000;

// 毫秒频率
const AVRational FF_FREQUENCY_Q = {FF_TIME_MS, 1};

// 毫秒时间间隔
const AVRational FF_TIME_BASE_Q = {1, FF_TIME_MS};

/**
 * @brief 设置log的回调函数，主要给android调试传参数
 * @param ptr: log的上下文
 * @param level: 日志级别
 * @param fmt: 日志格式
 * @param vl: 日志参数
 * @return 空
 */
static void avlog_callback(void *ptr, int level, const char *fmt, va_list vl) {
  DO_USE_VAR(ptr);
  if (level <= av_log_get_level()) {
#ifdef ANDROID
    __android_log_vprint(ANDROID_LOG_DEBUG, "fanplayer", fmt, vl);
#endif
  }
}

/**
 * @brief  中断的回调函数，avformat_open_input的操作是阻塞操作，如果不加以控制那么等待时间会达到30s以上
 * @return 产生错误
 * @ref https://www.cnblogs.com/shuiche/p/11983533.html
 */
static int interrupt_callback(void *param) {
  Player *player = (Player *)param;
  if (player->read_timeout == -1) {
    return 0;
  }
  return av_gettime_relative() - player->read_timelast > player->read_timeout
             ? AVERROR_EOF
             : 0;
}

/**
 * @brief 获得指定的type的流的总数
 * @param player: 播放器上下文
 * @param type: 指定的流的类型
 * @return 指定类型流的总数
 */
static int get_stream_total(Player *player, enum AVMediaType type) {
  int total = 0, i;
  for (i = 0; i < (int)player->avformat_context->nb_streams; i++) {
    if (player->avformat_context->streams[i]->codecpar->codec_type == type) {
      total++;
    }
  }
  return total;
}

/**
 * @brief 对于指定的流类型，除了当前的stream外，context上还有多少的type流
 * @param player: 播放器上下文
 * @param type: 指定的流的类型
 * @return 返回除了当前的astream上外，context上面还有多少数据流
 */
static int get_stream_current(Player *player, enum AVMediaType type) {
  int idx, cur, i;
  switch (type) {
    case AVMEDIA_TYPE_AUDIO:
      idx = player->astream_index;
      break;
    case AVMEDIA_TYPE_VIDEO:
      idx = player->vstream_index;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      return -1;
    default:
      return -1;
  }

  for (i = 0, cur = -1;
       i < (int)player->avformat_context->nb_streams && i != idx; i++) {
    if (player->avformat_context->streams[i]->codecpar->codec_type == type) {
      cur++;
    }
  }
  return cur;
}

/** 
 * @url https://www.cnblogs.com/leisure_chn/p/10429145.html 
 * @brief 初始化过滤器的图
 * @param player: 播放器上下文
 * @return 空
 */
static void vfilter_graph_init(Player *player) {
  const AVFilter *filter_src = avfilter_get_by_name("buffer");
  const AVFilter *filter_sink = avfilter_get_by_name("buffersink");
  AVCodecContext *vdec_ctx = player->vcodec_context;
  int video_rotate_rad = player->init_params.video_rotate * M_PI / 180;
  AVFilterInOut *inputs, *outputs;
  char temp[256], fstr[256];
  int ret;

  if (!player->vcodec_context || player->vfilter_graph) {
    return;
  }
  if (!player->init_params.video_deinterlace &&
      !player->init_params.video_rotate &&
      !*player->init_params.filter_string) {
    return;
  }

  player->vfilter_graph = avfilter_graph_alloc();
  if (!player->vfilter_graph) {
    return;
  }

  snprintf(temp, sizeof(temp),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
           vdec_ctx->width, vdec_ctx->height, vdec_ctx->pix_fmt,
           vdec_ctx->time_base.num, vdec_ctx->time_base.den,
           vdec_ctx->sample_aspect_ratio.num,
           vdec_ctx->sample_aspect_ratio.den);
  ret = avfilter_graph_create_filter(&player->vfilter_src_ctx, filter_src, "in",
                                     temp, NULL, player->vfilter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to create filter in");
    goto error_handler;
  }

  ret = avfilter_graph_create_filter(&player->vfilter_sink_ctx, filter_sink,
                                     "out", NULL, NULL, player->vfilter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to create filter out");
    goto error_handler;
  }

  if (player->init_params.video_rotate) {
    int ow = abs((int)(vdec_ctx->width * cos(video_rotate_rad))) +
             abs((int)(vdec_ctx->height * sin(video_rotate_rad)));
    int oh = abs((int)(vdec_ctx->width * sin(video_rotate_rad))) +
             abs((int)(vdec_ctx->height * cos(video_rotate_rad)));
    player->init_params.video_owidth = ow;
    player->init_params.video_oheight = oh;
    snprintf(temp, sizeof(temp), "rotate=%d*PI/180:%d:%d",
             player->init_params.video_rotate, ow, oh);
  }
  strcpy(fstr, player->init_params.video_deinterlace ? "yadif=0:-1:1" : "");
  strcat(fstr, player->init_params.video_deinterlace &&
                       player->init_params.video_rotate
                   ? "[0:v];[0:v]"
                   : ""); // 这里这个只是一个标记
  strcat(fstr, player->init_params.video_rotate ? temp : "");

  // filters_desc 最后一个标号未输出，默认为 “out”
  // filters_desc 第一个标号输出，默认为“in”
  inputs = avfilter_inout_alloc();
  outputs = avfilter_inout_alloc();
  inputs->name = av_strdup("out");
  inputs->filter_ctx = player->vfilter_sink_ctx;
  inputs->pad_idx = 0;
  inputs->next = NULL;
  outputs->name = av_strdup("in");
  outputs->filter_ctx = player->vfilter_src_ctx;
  outputs->pad_idx = 0;
  ret = avfilter_graph_parse_ptr(player->vfilter_graph,
                                 player->init_params.filter_string[0]
                                     ? player->init_params.filter_string
                                     : fstr,
                                 &inputs, &outputs, NULL);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0) {
    av_log(NULL, AV_LOG_WARNING, "failed to parse the avfilter !\n");
    goto error_handler;
  }

  ret = avfilter_graph_config(player->vfilter_graph, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_WARNING, "failed to config graph !\n");
    goto error_handler;
  }

error_handler:
  if (ret < 0) {
    avfilter_graph_free(&player->vfilter_graph); // 会把里面的filter也free
    player->vfilter_graph = NULL;
    player->vfilter_sink_ctx = NULL;
    player->vfilter_src_ctx = NULL;
  }
}

static void vfilter_graph_input(Player *player, AVFrame *frame) {
  int ret;
  if (player->vfilter_graph) {
    ret = av_buffersrc_add_frame(player->vfilter_src_ctx, frame);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "failed to add frame to src buffer !\n");
      return;
    }
  }
}

static int vfilter_graph_output(Player *player, AVFrame *frame) {
  return player->vfilter_graph
             ? av_buffersrc_add_frame(player->vfilter_sink_ctx, frame)
             : 0;
}

static void vfilter_graph_free(Player *player) {
  if (!player->vfilter_graph)
    return;
  avfilter_graph_free(&player->vfilter_graph);
  player->vfilter_graph = NULL;
  player->vfilter_sink_ctx = NULL;
  player->vfilter_src_ctx = NULL;
}

static int init_stream(Player *player, enum AVMediaType type, int sel) {
  if (!player) {
    av_log(NULL, AV_LOG_WARNING, "player is null");
    return -1;
  }

  const AVCodec *decoder = NULL;
  int idx = -1, cur = -1, i;

  for (i = 0; i < (int)player->avformat_context->nb_streams; i++) {
    if (player->avformat_context->streams[i]->codecpar->codec_type == type) {
      idx = i;
      if (++cur == sel) {
        break;
      }
    }
  }

  switch (type) {
    case AVMEDIA_TYPE_AUDIO:
      player->acodec_context = avcodec_alloc_context3(NULL);
      if (!player->acodec_context) {
        av_log(NULL, AV_LOG_WARNING, "failed to alloc context for audio");
        return -1;
      }

      decoder = avcodec_find_decoder(type);
      if (decoder &&
          avcodec_parameters_to_context(
              player->acodec_context,
              player->avformat_context->streams[idx]->codecpar) == 0 &&
          avcodec_open2(player->acodec_context, decoder, NULL) == 0) {
        player->astream_index = idx;
      } else {
        av_log(NULL, AV_LOG_WARNING, "failed to open audio decoder");
      }
      break;
    case AVMEDIA_TYPE_VIDEO:
      player->vcodec_context = avcodec_alloc_context3(NULL);
      if (!player->vcodec_context) {
        av_log(NULL, AV_LOG_WARNING, "failed to alloc context for video");
        return -1;
      }

      player->vstream_timebase =
          player->avformat_context->streams[idx]->time_base;

      if (player->init_params.video_hwaccel) {
#ifdef ANDROID
        // ffmpeg能够调用android端的mediacode去进行编解码(GPU)
        // 查看[这里](https://trac.ffmpeg.org/wiki/HWAccelIntro)
        switch (player->vcodec_context->codec_id) {
          case AV_CODEC_ID_H264:
            decoder = avcodec_find_encoder_by_name("h264_mediacodec");
            break;
          case AV_CODEC_ID_HEVC:
            decoder = avcodec_find_encoder_by_name("hevc_mediacodec");
            break;
          case AV_CODEC_ID_VP8:
            decoder = avcodec_find_encoder_by_name("vp8_mediacodec");
            break;
          case AV_CODEC_ID_VP9:
            decoder = avcodec_find_encoder_by_name("vp9_mediacodec");
            break;
          case AV_CODEC_ID_MPEG2VIDEO:
            decoder = avcodec_find_decoder_by_name("mpeg2_meidacodec");
            break;
          case AV_CODEC_ID_MPEG4:
            decoder = avcodec_find_decoder_by_name("mpeg4_mediacodec");
            break;
          default:
            break;
        }

        if (decoder &&
            avcodec_parameters_to_context(
                player->vcodec_context,
                player->avformat_context->streams[idx]->codecpar) == 0 &&
            avcodec_open2(player->vcodec_context, decoder, NULL) == 0) {
          player->vstream_index = idx;
          av_log(NULL, AV_LOG_WARNING,
                 "using android mediacodec hardware decoder %s !\n",
                 decoder->name);
        } else {
          decoder = NULL;
        }
        player->init_params.video_hwaccel = decoder ? 1 : 0;
#endif
      }
      if (!decoder) {
        if (player->init_params.video_thread_count > 0) {
          player->vcodec_context->thread_count =
              player->init_params.video_thread_count;
        }
        decoder = avcodec_find_decoder(type);

        if (decoder &&
            avcodec_parameters_to_context(
                player->vcodec_context,
                player->avformat_context->streams[idx]->codecpar) == 0 &&
            avcodec_open2(player->vcodec_context, decoder, NULL) == 0) {
          player->vstream_index = idx;
        } else {
          av_log(NULL, AV_LOG_WARNING, "failed to open audio decoder");
        }
        player->init_params.video_thread_count =
            player->vcodec_context->thread_count;
      }
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      return -1;
    default:
      return -1;
  }
  return 0;
}

/**
 * @brief 解析输入数据，解析的数据数据为下面的格式: 
 *      [key1 =(空格,:) value1;(,) key2 =(空格,:) value2;(,) key3 =(空格,:) value3 ...]
 * @param str: 要解析的字符串
 * @param key: 要解析的键
 * @param val：键值所对应的默认值
 * @param len: 字符串长度
 * @return 返回解析后的数据
 */
static char *parse_params(const char *str, const char *key, char *val,
                          int len) {
  char *p = (char *)strstr(str, key);
  int i;
  if (!p) {
    return NULL;
  }
  p += strlen(key);
  if (*p == '\0') {
    return NULL;
  }

  while (1) {
    if (*p != ' ' && *p != '=' && *p != ':') {
      break;
    } else
      p++;
  }

  for (i = 0; i < len; i++) {
    if (*p == ',' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\0') {
      break;
    } else if (*p == '\\') {
      p++;
    }
    val[i] = *p++;
  }
  val[i < len ? i : len - 1] = '\0';
  return val;
}

static int player_prepare_or_free(Player *player, int prepare) {
  char *url = player->url;
  AVInputFormat *fmt = NULL;

  AVDictionary *opts = NULL;
  int ret = -1;

  if (player->acodec_context) {
    avcodec_close(player->acodec_context);
    player->acodec_context = NULL;
  }
  if (player->vcodec_context) {
    avcodec_close(player->vcodec_context);
    player->vcodec_context = NULL;
  }
  if (player->avformat_context) {
    avformat_close_input(&player->avformat_context);
  }
  if (player->render) {
    render_close(player->render);
    player->render = NULL;
  }
  av_frame_unref(&player->aframe);
  player->aframe.pts = -1;
  av_frame_unref(&player->vframe);
  player->vframe.pts = -1;
  if (!prepare) {
    return 0;
  }

  // 确认rtsp和rtmp前面没有东西
  if (strstr(player->url, "rtsp://") == player->url ||
      strstr(player->url, "rtmp://")) {
    // 设置rtsp的播放的类型
    if (player->init_params.rtsp_transport) {
      av_dict_set(&opts, "rtsp_transport",
                  player->init_params.rtsp_transport == 1 ? "udp" : "tcp", 0);
    }
    av_dict_set(&opts, "buffer_size", "1048576", 0); // 设置buffer_size = 1MB
    av_dict_set(&opts, "probesize", "2", 0);         // 探测数据的步长
    av_dict_set(&opts, "analyzeduration", "5000000", 0); // 探测数据的时间

    // 如果是rtmp 开启同步，否则放弃音视频同步
    if (player->init_params.avts_syncmode == AVSYNC_MODE_AUTO) {
      player->init_params.avts_syncmode = memcmp(player->url, "rtmp://", 7) == 0
                                              ? AVSYNC_MODE_LIVE_SYNC1
                                              : AVSYNC_MODE_LIVE_SYNC0;
    }
  } else {
    if (player->init_params.avts_syncmode == AVSYNC_MODE_AUTO) {
      player->init_params.avts_syncmode = AVSYNC_MODE_FILE;
    }
  }

  if (player->init_params.video_vwidth != 0 &&
      player->init_params.video_vheight != 0) {
    char vsize[64];
    snprintf(vsize, sizeof(vsize), "%dx%d", player->init_params.video_vwidth,
             player->init_params.video_vheight);
    av_dict_set(&opts, "video_size", vsize, 0);
  }
  if (player->init_params.video_frame_rate != 0) {
    char frate[64];
    snprintf(frate, sizeof(frate), "%d", player->init_params.video_frame_rate);
    av_dict_set(&opts, "framerate", frate, 0);
  }

  while (1) {
    player->avformat_context = avformat_alloc_context();
    if (!player->avformat_context) {
      av_log(NULL, AV_LOG_ERROR, "failed to alloc the format context! \n");
      goto done;
    }

    player->avformat_context->interrupt_callback.callback = interrupt_callback;
    player->avformat_context->interrupt_callback.opaque = player;
    player->avformat_context->video_codec_id =
        player->init_params.video_codec_id;

    player->read_timelast = av_gettime_relative();
    player->read_timeout = player->init_params.init_timeout
                               ? av_rescale_q(player->init_params.init_timeout,
                                              FF_TIME_BASE_Q, AV_TIME_BASE_Q)
                               : -1;

    if (avformat_open_input(&player->avformat_context, url, fmt, &opts) != 0) {
      if (player->init_params.auto_reconnect > 0 &&
          !(player->status & PS_CLOSE)) {
        av_log(NULL, AV_LOG_INFO, "retry to open url: %s ...\n", url);
        av_usleep(100 * FF_TIME_MS);
      } else {
        av_log(NULL, AV_LOG_ERROR, "failed to open url: %s !\n", url);
        goto done;
      }
    } else {
      av_log(NULL, AV_LOG_DEBUG, "successed to open url: %s\n", url);
      break;
    }
  }

  if (avformat_find_stream_info(player->avformat_context, NULL) < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to find stream info !\n");
    goto done;
  }

  player->astream_index = -1;
  init_stream(player, AVMEDIA_TYPE_AUDIO, player->init_params.audio_stream_cur);
  player->vstream_index = -1;
  init_stream(player, AVMEDIA_TYPE_VIDEO, player->init_params.video_stream_cur);
  if (player->astream_index != -1) {
    player->seek_req |= PS_A_SEEK;
  }
  if (player->vstream_index != -1) {
    player->seek_req |= PS_V_SEEK;
  }

  if (player->vstream_index != -1) {
    player->vfrate =
        player->avformat_context->streams[player->vstream_index]->r_frame_rate;
    if (av_q2d(player->vfrate) > 100.f) {
      player->vfrate.num = 20;
      player->vfrate.den = 1;
    }
    player->init_params.video_vwidth = player->init_params.video_owidth =
        player->vcodec_context->width;
    player->init_params.video_vheight = player->init_params.video_owidth =
        player->vcodec_context->height;
    vfilter_graph_init(player);
  }

  player->cmnvars.start_time = av_rescale_q(
      player->avformat_context->start_time, AV_TIME_BASE_Q, FF_TIME_BASE_Q);
  player->cmnvars.apts =
      player->astream_index != -1 ? player->cmnvars.start_time : -1;
  player->cmnvars.vpts =
      player->vstream_index != -1 ? player->cmnvars.start_time : -1;

  player->render =
      render_open(player->init_params.adev_render_type,
                  player->init_params.vdev_render_type, player->cmnvars.winmsg,
                  player->vfrate, player->init_params.video_owidth,
                  player->init_params.video_oheight, &player->cmnvars);

  if (player->vstream_index == -1) {
    int effect = VISUAL_EFFECT_WAVEFORM;
    render_setparam(player->render, PARAM_VISUAL_EFFECT, &effect);
  }

  player->init_params.video_frame_rate =
      player->vfrate.num / player->vfrate.den;
  player->init_params.video_stream_totol =
      get_stream_total(player, AVMEDIA_TYPE_VIDEO);
  player->init_params.audio_channels =
      player->acodec_context ? av_get_channel_layout_nb_channels(
                                   player->acodec_context->channel_layout)
                             : -1;
  player->init_params.audio_sample_rate =
      player->acodec_context ? player->acodec_context->sample_rate : -1;
  player->init_params.audio_stream_total =
      get_stream_total(player, AVMEDIA_TYPE_AUDIO);
  player->init_params.subtitle_stream_total =
      get_stream_total(player, AVMEDIA_TYPE_SUBTITLE);
  player->init_params.video_codec_id = player->avformat_context->video_codec_id;

  ret = 0;
done:
  player_send_message(player->cmnvars.winmsg,
                      ret ? MSG_OPEN_FAILED : MSG_OPEN_DONE, player);
  if (ret == MSG_OPEN_DONE && player->init_params.open_autoplay) {
    player_play(player);
  }
  return ret;
}

static int handle_fseek_or_reconnect(Player *player) {
  int pause_req = 0, pause_ack = 0, ret = 0;

  if (player->avformat_context &&
      (player->status & (PS_F_SEEK | PS_RECONNECT)) == 0) {
    return 0;
  }
  if (player->astream_index != -1) {
    pause_req |= PS_A_PAUSE;
    pause_ack |= PS_A_PAUSE << 16;
  }
  if (player->vstream_index != -1) {
    pause_req |= PS_V_PAUSE;
    pause_ack |= PS_V_PAUSE << 16;
  }

  // make render run
  render_pause(player->render, 0);

  pthread_mutex_lock(&player->lock);
  player->status |= pause_req | player->seek_req;
  pthread_mutex_unlock(&player->lock);

  while ((player->status & pause_ack) != pause_ack) {
    if (player->status & PS_CLOSE) {
      return 0;
    }
    av_usleep(20 * FF_TIME_MS);
  }

  if (!player->avformat_context || (player->status & PS_RECONNECT)) {
    if (ret == 0) {
      player_send_message(player->cmnvars.winmsg, MSG_STREAM_DISCONNECT,
                          player);
    }
    ret = player_prepare_or_free(player, 1);
    if (ret == 0) {
      player_send_message(player->cmnvars.winmsg, MSG_STREAM_CONNECTED, player);
    }
  } else {
    av_seek_frame(player->avformat_context, player->seek_sidx, player->seek_pos,
                  AVSEEK_FLAG_BACKWARD);
    if (player->astream_index != -1) {
      avcodec_flush_buffers(player->acodec_context);
    }
    if (player->vstream_index != -1) {
      avcodec_flush_buffers(player->vcodec_context);
    }
  }

  pktqueue_reset(player->pktqueue); // reset pktqueue

  // make audio & video decoding thread resume
  pthread_mutex_lock(&player->lock);
  if (ret == 0)
    player->status &= ~(PS_F_SEEK | PS_RECONNECT | pause_req | pause_ack);
  pthread_mutex_unlock(&player->lock);
  return ret;
}

void *player_open(char *file, void *win, PlayerInitParams *params) {
  Player *player = (Player *)calloc(1, sizeof(Player));
  if (!player) {
    return NULL;
  }

  // 注册设备 和 初始化网络
  avdevice_register_all();
  avformat_network_init();

  av_log_set_level(AV_LOG_WARNING);
  av_log_set_callback(avlog_callback);

  pthread_mutex_init(&player->lock, NULL);
  player->status = (PS_A_PAUSE | PS_V_PAUSE | PS_R_PAUSE); // 获取状态

  player->pktqueue = pktqueue_create(0, &player->cmnvars);
  if (player->pktqueue) {
    av_log(NULL, AV_LOG_ERROR, "failed to create packet queue !\n");
    goto error_handler;
  }

  if (params) {
    memcpy(&player->init_params, params, sizeof(PlayerInitParams));
  }
  player->cmnvars.init_params = &player->init_params;

  strcpy(player->url, file);

#ifdef ANDROID
  player->cmnvars.winmsg = JniRequestWinObj(win);
#endif

  pthread_create(&player->avdemux_thread, NULL, av_demux_thread_proc, player);

  pthread_create(&player->adecode_thread, NULL, audio_decode_thread_proc,
                 player);
  pthread_create(&player->vdecode_thread, NULL, video_decode_thread_proc,
                 player);

  return player;

error_handler:
  player_close(player);
  return NULL;
}

void player_play(void *ctxt) {
  Player *player = ctxt;
  if (!player || !player->avformat_context) {
    return;
  }
  pthread_mutex_lock(&player->lock);
  player->status &= PS_CLOSE;
  pthread_mutex_unlock(&player->lock);
  render_pause(player->render, 0);
  datarate_reset(player->datarate);
}

void player_pause(void *hplayer) {
  Player *player = (Player *)hplayer;
  if (!player) { return; }
  pthread_mutex_lock(&player->lock);
  player->status |= PS_R_PAUSE;
  pthread_mutex_unlock(&player->lock);
  render_pause(player->render, 1);
  datarate_reset(player->datarate);
}

void player_send_message(void *extra, int32_t msg, void *param) {
#ifdef ANDROID
  JniPostMessage(extra, msg, param);
#else
  DO_USE_VAR(extra);
  DO_USE_VAR(msg)
  DO_USE_VAR(param);
#endif
}

void player_close(void *ctxt) {
  Player *player = (Player *)ctxt;
  if (!player) {
    return;
  }

  player->read_timeout = 0;
  pthread_mutex_lock(&player->lock);
  player->status |= PS_CLOSE;
  pthread_mutex_unlock(&player->lock);
  render_pause(player->render, 2); // TODO: ?
  if (player->adecode_thread) {
    pthread_join(player->adecode_thread, NULL);
  }
  if (player->vdecode_thread) {
    pthread_join(player->vdecode_thread, NULL);
  }
  if (player->avdemux_thread) {
    pthread_join(player->avdemux_thread, NULL);
  }
  pthread_mutex_destroy(&player->lock);

  player_prepare_or_free(player, 0);
  recorder_free(player->recorder);
  datarate_destroy(player->datarate);
  pktqueue_destroy(player->pktqueue);

#ifdef ANDROI
  JniReleaseWinObj(player->cmnvars.winmsg);
#endif

  free(player);

  avformat_network_deinit();
}

void *av_demux_thread_proc(void *ctxt) {
  Player *player = (Player *)ctxt;
  AVPacket *packet = NULL;
  int ret = 0;

  if (!player) {
    return NULL;
  }

  while (!(player->status & PS_CLOSE)) {
    if ((packet = pktqueue_request_packet(player->pktqueue)) == NULL) {
      continue;
    }

    ret = av_read_frame(player->avformat_context, packet);
    if (ret < 0) { // TODO: 这里需要刷出最后几帧的数据吗
      pktqueue_release_packet(player->pktqueue, packet);
      if (player->init_params.auto_reconnect > 0 &&
          av_gettime_relative() - player->read_timelast >
              av_rescale_q(player->init_params.auto_reconnect, FF_TIME_BASE_Q,
                           AV_TIME_BASE_Q)) {
        pthread_mutex_lock(&player->lock);
        player->status |= PS_RECONNECT;
        pthread_mutex_unlock(&player->lock);
      }
      av_usleep(20 * FF_TIME_MS); // sleep 20 ms
    } else {
      player->read_timelast = av_gettime_relative();
      if (packet->stream_index == player->astream_index) {
        recorder_packet(player->recorder, packet);
        pktqueue_audio_enqueue(player->pktqueue, packet);
      }

      if (packet->stream_index == player->vstream_index) {
        recorder_packet(player->recorder, packet);
        pktqueue_video_enqueue(player->pktqueue, packet);
      }

      if (packet->stream_index != player->astream_index &&
          packet->stream_index != player->vstream_index) {
        pktqueue_release_packet(player->pktqueue, packet);
      }
    }
  }

#ifdef ANROID
  JniDetachCurrentThread();
#endif
  return NULL;
}

int decoder_decode_frame(void *ctxt, void *pkt, void *frm, void *got) {
  AVCodecContext *context = (AVCodecContext *)ctxt;
  AVFrame *frame = (AVFrame *)frm;
  AVPacket *packet = (AVPacket *)pkt;
  int *got_frame = (int *)got;
  int retr = 0, rets = 0;
  if (!context) {
    return -1;
  }

  *got_frame = 0;
  do {
    retr = avcodec_receive_frame(context, frame);
    if (retr >= 0) {
      *got_frame = 1;
      return 0;
    } else if (retr == AVERROR_EOF) {
      avcodec_flush_buffers(context);
      return 0;
    } else if (retr == AVERROR(EAGAIN)) {
      break;
    } else {
      return -1;
    }
  } while (1);

  do {
    rets = avcodec_send_packet(context, packet);
    if (rets == AVERROR(EAGAIN) && retr == AVERROR(EAGAIN)) {
      av_log(NULL, AV_LOG_WARNING, "error api happend!");
      av_usleep(1 * FF_TIME_MS);
      continue;
    } else {
      break;
    }
  } while (1);

  if (rets < 0) {
    return -1;
  }

  return 0;
}

void *video_decode_thread_proc(void *ctxt) {
  Player *player = (Player *)ctxt;
  AVPacket *packet = NULL;
  int ret, got;
  if (!player) {
    return NULL;
  }

  while (!(player->status & PS_CLOSE)) {
    while (player->status & PS_V_PAUSE) {
      pthread_mutex_lock(&player->lock);
      player->status |= (PS_V_PAUSE << 16); // TODO: 特殊标记
      pthread_mutex_unlock(&player->lock);
      av_usleep(20 * FF_TIME_MS); // 20 ms
      continue;
    }

    if (!packet && player->vframe.width && player->vframe.height &&
        *player->vframe.data) {
      render_video(player->render, &player->vframe);
    } // 只有一帧的进行渲染，一般出现在mp3

    if (!(packet = pktqueue_video_dequeue(player))) {
      continue;
    }
    datarate_video_packet(player->datarate, packet);

    // avcodec_decode_video2 已经被丢弃，因为对于一个packet只能解码一帧
    while (packet && packet->size > 0 &&
           !(player->status & (PS_V_PAUSE | PS_CLOSE))) {
      ret = decoder_decode_frame(player->vcodec_context, packet,
                                 &player->vframe, &got);
      if (ret < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "an error occurred during decoding video. \n");
        break;
      }

      if (player->vcodec_context->width != player->init_params.video_vwidth ||
          player->vcodec_context->height != player->init_params.video_vheight) {
        player->init_params.video_vwidth = player->init_params.video_owidth =
            player->vcodec_context->width;
        player->init_params.video_vheight = player->init_params.video_oheight =
            player->vcodec_context->height;
        player_send_message(player->cmnvars.winmsg, MSG_VIDEO_RESIZED, NULL);
      }

      if (got) {
        // 是否加锁
        player->vframe.height = player->vcodec_context->height;
        vfilter_graph_input(player, &player->vframe);
        do {
          if (vfilter_graph_output(player, &player->vframe) < 0) {
            break;
          }
          player->seek_vpts =
              player->vframe.best_effort_timestamp; // 读到的帧锁在pts
          // 这里很特殊，从微妙转化为毫秒
          // player->vframe.pts = av_rescale_q(player->seek_vpts, player->vstream_timebase, FF_TIME_BASE_Q);
          // 这里是seek的逻辑，就是如果seek的话，当前的pts - seek_dest < diff, 那么会一直循环，知道进入将PS_V_SEEK解开
          int64_t vframe_pts = av_rescale_q(
              player->seek_vpts, player->vstream_timebase, FF_TIME_BASE_Q);
          if (player->status & PS_V_SEEK) {
            if (player->seek_dest - vframe_pts <= player->seek_diff) {
              player->cmnvars.start_tick = av_rescale_q(
                  av_gettime_relative(), AV_TIME_BASE_Q, FF_TIME_BASE_Q);
              player->cmnvars.start_pts = vframe_pts;
              player->cmnvars.vpts = vframe_pts;
              player->cmnvars.apts =
                  player->astream_index == -1 ? -1 : player->seek_dest;
              pthread_mutex_lock(&player->lock);
              player->status &= ~PS_V_SEEK;
              pthread_mutex_unlock(&player->lock);
              if (player->status & PS_R_PAUSE) {
                render_pause(player->render, 1);
              }
            }
          }
          if (!(player->status & PS_V_SEEK)) {
            render_video(player->render, &player->vframe);
          }
        } while (player->vfilter_graph);
      } else {
        break;
      }
    }
    pktqueue_release_packet(player->pktqueue,
                            packet); // 将帧放入队列中，这里到达了末尾
  }

#ifdef ANDROID
  JniDetachCurrentThread();
#endif
  return NULL;
}

void *audio_decode_thread_proc(void *ctxt) {
  Player *player = (Player *)ctxt;
  AVPacket *packet = NULL;
  int64_t apts;
  int ret, got;
  if (!player) {
    return NULL;
  }

  while (!(player->status & PS_CLOSE)) {
    if (player->status & PS_A_PAUSE) {
      pthread_mutex_lock(&player->lock);
      player->status |= (PS_A_PAUSE << 16);
      pthread_mutex_lock(&player->lock);
      av_usleep(20 * FF_TIME_MS); // 20 ms
      continue;
    }

    if (!(packet = pktqueue_video_dequeue(player))) {
      continue;
    }
    datarate_audio_packet(player->datarate, packet);

    apts = AV_NOPTS_VALUE;
    while (packet && packet->size > 0 &&
           !(player->status & (PS_A_PAUSE | PS_CLOSE))) {
      ret = decoder_decode_frame(player->acodec_context, packet,
                                 &player->aframe, &got);
      if (ret < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "an error occurred during decoding audio. \n");
        break;
      }

      if (got) {
        AVRational tb_sample_rate = {1, player->acodec_context->sample_rate};
        // 从stream时间基转为codec时间基(stream 时间基一般为25HZ，而code时间基可能为448000HZ，因此要做转化)
        if (apts == AV_NOPTS_VALUE) {
          apts = av_rescale_q(player->aframe.pts, player->astream_timebase,
                              tb_sample_rate);
        } else {
          apts += player->aframe.nb_samples; // 因为一帧为nb_samples
        }

        // 将从微妙转化为毫秒
        player->aframe.pts = av_rescale_q(apts, tb_sample_rate, FF_TIME_BASE_Q);

        if (player->status & PS_A_SEEK) {
          // 当seek_dest 和 pts 相差在规定范围内时，就seek
          if (player->seek_dest - player->aframe.pts <= player->seek_diff) {
            player->cmnvars.start_tick = av_rescale_q(
                av_gettime_relative(), AV_TIME_BASE_Q, FF_TIME_BASE_Q);
            player->cmnvars.start_pts = player->aframe.pts;
            player->cmnvars.apts = player->aframe.pts;
            player->cmnvars.vpts =
                player->vstream_index == -1 ? -1 : player->seek_dest;
            pthread_mutex_lock(&player->lock);
            player->status &= ~PS_A_SEEK;
            pthread_mutex_unlock(&player->lock);
            if (player->status & PS_R_PAUSE) {
              render_pause(player->render, 1);
            }
          }
        }

        if (!(player->status & PS_A_SEEK)) {
          render_audio(player->render, &player->aframe);
        }
      } else {
        break;
      }
    }
    pktqueue_release_packet(player->pktqueue, packet);
  }

#ifdef ANDROID
  JniDetachCurrentThread();
#endif

  return NULL;
}

void player_seek(void *hplayer, int64_t ms, int type) {
  Player *player = (Player *)hplayer;
  if (!player) {
    return;
  }

  if (player->status & (PS_F_SEEK | player->seek_req)) {
    av_log(NULL, AV_LOG_WARNING, "seek busy !\n");
    return;
  }

  switch (type) {
    case SEEK_STEP_FORWARD:
      render_pause(player->render, 1);
      render_setparam(player->render, PARAM_RENDER_STEPFORWARD, NULL);
      return;
    case SEEK_STEP_BACKWARD:
      player->seek_dest =
          av_rescale_q(player->seek_vpts, player->vstream_timebase,
                       AV_TIME_BASE_Q) -
          FF_TIME_MS * player->vfrate.den / player->vfrate.num - 1;
      player->seek_pos =
          player->seek_vpts +
          av_rescale_q(ms, FF_TIME_BASE_Q, player->vstream_timebase);
      player->seek_diff = 0;
      player->seek_sidx = player->vstream_index;
      pthread_mutex_lock(&player->lock);
      player->status |= PS_R_PAUSE;
      pthread_mutex_unlock(&player->lock);
      break;
    default:
      player->seek_dest = player->cmnvars.start_time + ms;
      player->seek_pos = av_rescale_q(player->cmnvars.start_time + ms,
                                      FF_TIME_BASE_Q, AV_TIME_BASE_Q);
      player->seek_diff = 100;
      player->seek_sidx = -1;
      break;
  }

  pthread_mutex_lock(&player->lock);
  player->status |= PS_F_SEEK;
  pthread_mutex_unlock(&player->lock);
}

int player_snapshot(void *hplayer, char *file, int w, int h, int wait_time) {
  Player *player = (Player *)hplayer;
  if (!hplayer) {
    return -1;
  }
  return player->vstream_index == -1
             ? -1
             : render_snapshot(player->render, file, w, h, wait_time);
}

void player_load_params(PlayerInitParams *params, char *str) {
  char value[16];
  params->video_stream_cur =
      atoi(parse_params(str, "video_stream_cur", value, sizeof(value)) ? value
                                                                       : "0");
  params->video_thread_count =
      atoi(parse_params(str, "video_thread_count", value, sizeof(value)) ? value
                                                                         : "0");
  params->video_hwaccel = atoi(
      parse_params(str, "video_hwaccel", value, sizeof(value)) ? value : "0");
  params->video_deinterlace =
      atoi(parse_params(str, "video_deinterlace", value, sizeof(value)) ? value
                                                                        : "0");
  params->video_rotate = atoi(
      parse_params(str, "video_rotate", value, sizeof(value)) ? value : "0");
  params->video_bufpktn = atoi(
      parse_params(str, "video_bufpktn", value, sizeof(value)) ? value : "0");
  params->video_vwidth = atoi(
      parse_params(str, "video_vwidth", value, sizeof(value)) ? value : "0");
  params->video_vheight = atoi(
      parse_params(str, "video_vheight", value, sizeof(value)) ? value : "0");
  params->audio_stream_cur =
      atoi(parse_params(str, "audio_stream_cur", value, sizeof(value)) ? value
                                                                       : "0");
  params->audio_bufpktn = atoi(
      parse_params(str, "audio_bufpktn", value, sizeof(value)) ? value : "0");
  params->subtitle_stream_cur = atoi(
      parse_params(str, "subtitle_stream_cur", value, sizeof(value)) ? value
                                                                     : "0");
  params->vdev_render_type =
      atoi(parse_params(str, "vdev_render_type", value, sizeof(value)) ? value
                                                                       : "0");
  params->adev_render_type =
      atoi(parse_params(str, "adev_render_type", value, sizeof(value)) ? value
                                                                       : "0");
  params->adev_render_type =
      atoi(parse_params(str, "adev_render_type", value, sizeof(value)) ? value
                                                                       : "0");
  params->init_timeout = atoi(
      parse_params(str, "init_timeout", value, sizeof(value)) ? value : "0");
  params->open_autoplay = atoi(
      parse_params(str, "open_autoplay", value, sizeof(value)) ? value : "0");
  params->auto_reconnect = atoi(
      parse_params(str, "auto_reconnect", value, sizeof(value)) ? value : "0");
  params->rtsp_transport = atoi(
      parse_params(str, "rtsp_transport", value, sizeof(value)) ? value : "0");
  params->avts_syncmode = atoi(
      parse_params(str, "avts_syncmode", value, sizeof(value)) ? value : "0");
  params->swscale_type = atoi(
      parse_params(str, "swscale_type", value, sizeof(value)) ? value : "0");
  parse_params(str, "filter_string", params->filter_string,
               sizeof(params->filter_string));
  parse_params(str, "ffrdp_tx_key", params->ffrdp_tx_key,
               sizeof(params->ffrdp_tx_key));
  parse_params(str, "ffrdp_rx_key", params->ffrdp_rx_key,
               sizeof(params->ffrdp_rx_key));
}

void player_setparam(void *hplayer, int id, void *param) {
  Player *player = (Player *)hplayer;
  if (!hplayer) {
    return;
  }
  render_setparam(player->render, id, param);
}

void player_getparam(void *hplayer, int id, void *param) {
  Player *player = (Player *)hplayer;
  if (!hplayer || !param) {
    return;
  }

  switch (id) {
    case PARAM_MEDIA_DURATION:
      *(int64_t *)param = player->avformat_context
                              ? av_rescale_q(player->avformat_context->duration,
                                             AV_TIME_BASE_Q, FF_TIME_BASE_Q)
                              : 1;
      break;
    case PARAM_MEDIA_POSITION:
      if ((player->status & PS_F_SEEK) || (player->status & player->seek_pos)) {
        *(int64_t *)param = player->seek_dest - player->cmnvars.start_time;
      } else {
        int64_t pos = 0;
        render_getparam(player->render, id, &pos);
        *(int64_t *)param = pos == -1 ? -1 : pos - player->cmnvars.start_time;
      }
      break;
    case PARAM_VIDEO_WIDTH:
      if (!player->vcodec_context) {
        *(int *)param = 0;
      } else {
        *(int *)param = player->init_params.video_owidth;
      }
      break;
    case PARAM_VIDEO_HEIGHT:
      if (!player->vcodec_context) {
        *(int *)param = 0;
      } else {
        *(int *)param = player->init_params.video_oheight;
      }
      break;
    case PARAM_RENDER_GET_CONTEXT:
      *(void **)param = player->render;
      break;
    case PARAM_PLAYER_INIT_PARAMS:
      memcpy(param, &player->init_params, sizeof(PlayerInitParams));
      break;
    case PARAM_DATARATE_VALUE:
      if (!player->datarate) {
        player->datarate = datarate_create();
      }
      datarate_result(player->datarate, NULL, NULL, (int *)param);
      break;
    default:
      render_getparam(player->render, id, param);
      break;
  }
}
