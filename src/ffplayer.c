#include "ffplayer.h"

#include <pthread.h>
#include <string.h>
#include <limits.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/frame.h>

#include "stdefine.h"
#include "datarate.h"
#include "pktqueue.h"
#include "recorder.h"
#include "ffrender.h"

#ifdef ANDROID
#include "ddgplayer_jni.h"
#endif



typedef struct {
  // muxer format
  AVFormatContext* avformat_context;

  // audio
  AVCodecContext* acodec_context;
  int32_t astream_index;
  AVRational astream_timebase;
  AVFrame aframe;

  // video
  AVCodecContext* vcodec_context;
  int32_t vstream_index;
  AVRational vstream_timebase; // 时间单位
  AVFrame vframe;

  // queue
  void* pktqueue; // 队列
  void* render; // 渲染器
  void* datarate; // 码率

  // thread status
  #define PS_A_PAUSE (1 << 0) // audio decoding pause
  #define PS_V_PAUSE (1 << 1) // video decoding pause
  #define PS_R_PAUSE (1 << 2) // render pause
  #define PS_F_SEEK (1 << 3)
  #define PS_A_SEEK (1 << 4)
  #define PS_V_SEEK (1 << 5)
  #define PS_RECONNECT (1 << 6)
  #define PS_CLOSE (1 << 7)
  int status;

  // seek
  int seek_req;
  int64_t seek_pos;
  int64_t seek_dest;
  int64_t seek_vpts;

  int seek_diff;
  int seek_sidx; // TODO:

  // player common vars
  CommonVars cmnvars;

  pthread_mutex_t lock;
  pthread_t avdemux_thread;
  pthread_t adecode_thread;
  pthread_t vdecode_thread;

  AVFilterGraph* vfilter_graph;
  AVFilterContext* vfilter_src_ctx;
  AVFilterContext* vfilter_sink_ctx;

   // player init timeout, and init params
  int64_t read_timelast; // 上一次读取的时间，主要用于音视频同步(微秒)
  int64_t read_timeout; // 读取是否超时
  PlayerInitParams init_params;

  // save url
  char url[PATH_MAX];

  // recoder used for recording
  void* recorder;

} Player;


static const AVRational TIMEBASE_MS = {1, 1000};

static void avlog_callback(void* ptr, int level, const char* fmt,  va_list vl) {
  DO_USE_VAR(ptr);  // TODO: ?
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
**/
static int interrupt_callback(void* param) {
  Player* player = (Player*)param;
  if (player->read_timeout == -1) {
    return 0;
  }
  return av_gettime_relative() - player->read_timelast > player->read_timeout
             ? AVERROR_EOF
             : 0;
}

static int get_stream_total(Player* player, enum AVMediaType type) {
  int total = 0, i;
  for (i = 0; i < (int)player->avformat_context->nb_streams; i++) {
    if (player->avformat_context->streams[i]->codecpar->codec_type == type) {
      total++;
    }
  }
  return total;
}

// https://www.cnblogs.com/leisure_chn/p/10429145.htmlq:w
static void vfilter_graph_init(Player* player) {
  const AVFilter* filter_src = avfilter_get_by_name("buffer");
  const AVFilter* filter_sink = avfilter_get_by_name("buffersink");
  AVCodecContext* vdec_ctx = player->vcodec_context;
  int video_rotate_rad = player->init_params.video_rotate * M_PI / 180;
  AVFilterInOut* inputs, *outputs;
  char temp[256], fstr[256];
  int ret;

  if (!player->vcodec_context || player->vfilter_graph) { return; }
  if (!player->init_params.video_deinterlace && !player->init_params.video_rotate && 
      !*player->init_params.filter_string) {
    return;
  }

  player->vfilter_graph = avfilter_graph_alloc();
  if (!player->vfilter_graph) { return; }

  snprintf(temp, sizeof(temp), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", 
      vdec_ctx->width, vdec_ctx->height, vdec_ctx->pix_fmt, 
      vdec_ctx->time_base.num, vdec_ctx->time_base.den, vdec_ctx->sample_aspect_ratio.num, 
      vdec_ctx->sample_aspect_ratio.den);
  ret = avfilter_graph_create_filter(&player->vfilter_src_ctx, filter_src, "in", temp, NULL, player->vfilter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to create filter in");
    goto error_handler;
  }

  ret = avfilter_graph_create_filter(&player->vfilter_sink_ctx, filter_sink, "out", NULL, NULL, player->vfilter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to create filter out");
    goto error_handler;
  }

  if (player->init_params.video_rotate) {
    int ow = abs((int)(vdec_ctx->width * cos(video_rotate_rad))) + abs((int)(vdec_ctx->height * sin(video_rotate_rad)));
    int oh = abs((int)(vdec_ctx->width * sin(video_rotate_rad))) + abs((int)(vdec_ctx->height * cos(video_rotate_rad)));
    player->init_params.video_owidth = ow;
    player->init_params.video_oheight = oh;
    snprintf(temp, sizeof(temp), "rotate=%d*PI/180:%d:%d", player->init_params.video_rotate, ow, oh);
  }
  strcpy(fstr, player->init_params.video_deinterlace ? "yadif=0:-1:1" : "");
  strcat(fstr, player->init_params.video_deinterlace && player->init_params.video_rotate ? "[0:v];[0:v]" : ""); // 这里这个只是一个标记
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
      player->init_params.filter_string[0] ? player->init_params.filter_string : fstr, &inputs, &outputs, NULL);
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

static void vfilter_graph_input(Player* player, AVFrame* frame) {
  int ret;
  if (player->vfilter_graph) {
    ret = av_buffersrc_add_frame(player->vfilter_src_ctx, frame);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "failed to add frame to src buffer !\n"); 
      return;
    }
  }
}

static int vfilter_graph_output(Player* player, AVFrame* frame) {
  return player->vfilter_graph ? av_buffersrc_add_frame(player->vfilter_sink_ctx, frame) : 0;
}

static void vfilter_graph_free(Player* player) {
  if (!player->vfilter_graph) return;
  avfilter_graph_free(&player->vfilter_graph);
  player->vfilter_graph = NULL;
  player->vfilter_sink_ctx = NULL;
  player->vfilter_src_ctx = NULL;
}

static int handle_fseek_or_reconnect(Player* player) {
  return 0;
}

static int player_prepare_or_free(Player* player, int prepare) {

#define AVDEV_DSHOW "dshow" // 音视频采集设备
#define AVDEV_GDIGRAB "gdigrab" // 左面采集设备
#define AVDEV_VFWCAP "vfwcap" // 视频采集设备

  // char* url = player->url;
  // AVInputFormat* fmt = NULL;
  // AVDictionary* opts = NULL;
  int ret = -1;

  if (player->acodec_context) { avcodec_close(player->acodec_context); player->acodec_context = NULL; }
  if (player->vcodec_context) { avcodec_close(player->vcodec_context); player->vcodec_context = NULL; }
  if (player->avformat_context) { avformat_close_input(&player->avformat_context); }
  if (player->render) { render_close(player->render); player->render = NULL; }
  av_frame_unref(&player->aframe); player->aframe.pts = -1;
  av_frame_unref(&player->vframe); player->vframe.pts = -1;
  if (!prepare) { return 0; }  
  
  // TODO:
  return ret;
}

void* av_demux_thread_proc(void* ctxt) {
  Player* player = (Player*)ctxt;
  AVPacket* packet = NULL;
  int ret = 0;

  if (!player) { return NULL; }

  while (!(player->status & PS_CLOSE)){
    if ((packet = pktqueue_request_packet(player->pktqueue)) == NULL) { continue; }

    ret = av_read_frame(player->avformat_context, packet);
    if (ret < 0) { // TODO: 这里需要刷出最后几帧的数据吗
      pktqueue_release_packet(player->pktqueue, packet);
      if (player->init_params.auto_reconnect > 0 && 
          av_gettime_relative() - player->read_timelast > player->init_params.auto_reconnect * 1000) { // TODO: 整合时间
        pthread_mutex_lock(&player->lock);        
        player->status |= PS_RECONNECT;
        pthread_mutex_unlock(&player->lock);
      }
      av_usleep(20 * 1000); // sleep 20 ms
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


int decoder_decode_frame(void* ctxt, void* pkt, void* frm, void* got) {
  AVCodecContext* context = (AVCodecContext*)ctxt;
  AVFrame* frame = (AVFrame*) frm;
  AVPacket* packet = (AVPacket*) pkt;
  int* got_frame = (int*)got;
  int retr = 0, rets = 0;
  if (!context) { return -1; }

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
      av_usleep(1 * 1000);   
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

void player_send_message(void* extra, int32_t msg, void* param) {
#ifdef ANDROID
  JniPostMessage(extra, msg, param);
#else
  DO_USE_VAR(extra);
  DO_USE_VAR(msg)
  DO_USE_VAR(param);
#endif
}


void* audio_decode_thread_proc(void* ctxt) {
  Player* player = (Player*)ctxt;
  AVPacket* packet = NULL;
  int64_t apts;
  int ret, got;
  if (!player) { return NULL; }

  while (!(player->status & PS_CLOSE)) {
    if (player->status & PS_A_PAUSE) {
      pthread_mutex_lock(&player->lock);
      player->status |= (PS_A_PAUSE << 16);
      pthread_mutex_lock(&player->lock);
      av_usleep(20 * 1000); // 20 ms
      continue;
    }

    if (!(packet = pktqueue_video_dequeue(player))) { continue; }
    datarate_audio_packet(player->datarate, packet);

    apts = AV_NOPTS_VALUE;
    while (packet && packet->size > 0 && !(player->status & (PS_A_PAUSE | PS_CLOSE))) {
      ret = decoder_decode_frame(player->acodec_context, packet, &player->aframe, &got); 
      if (ret < 0) {
        av_log(NULL, AV_LOG_WARNING, "an error occurred during decoding audio. \n");
        break;
      }

      if (got) {
        AVRational tb_sample_rate = {1, player->acodec_context->sample_rate};
        // 从stream时间基转为codec时间基(stream 时间基一般为25HZ，而code时间基可能为448000HZ，因此要做转化)
        if (apts == AV_NOPTS_VALUE) {
          apts = av_rescale_q(player->aframe.pts, player->astream_timebase, tb_sample_rate);
        } else {
          apts += player->aframe.nb_samples; // 因为一帧为nb_samples
        }
        // 将从微妙转化为毫秒
        player->aframe.pts = av_rescale_q(apts, tb_sample_rate, TIMEBASE_MS); 

        if (player->status & PS_A_SEEK) {
          // 当seek_dest 和 pts 相差在规定范围内时，就seek
          if (player->seek_dest - player->aframe.pts <= player->seek_diff) {
            player->cmnvars.start_tick = av_gettime_relative() / 1000;
            player->cmnvars.start_pts = player->aframe.pts;
            player->cmnvars.apts = player->aframe.pts;
            player->cmnvars.vpts = player->vstream_index == -1 ? -1 : player->seek_dest;
            pthread_mutex_lock(&player->lock);
            player->status &= ~PS_A_SEEK;
            pthread_mutex_unlock(&player->lock);
            if (player->status & PS_R_PAUSE) { render_pause(player->render, 1); }
          }
        }

        if (!(player->status & PS_A_SEEK)) { render_audio(player->render, &player->aframe); }
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


void* video_decode_thread_proc(void* ctxt) {
  Player* player = (Player*)ctxt;
  AVPacket* packet = NULL;
  int ret, got;
  if (!player) { return NULL; }

  while (!(player->status & PS_CLOSE)) {
    while (player->status & PS_V_PAUSE) {
      pthread_mutex_lock(&player->lock);
      player->status |= (PS_V_PAUSE << 16); // TODO: 特殊标记
      pthread_mutex_unlock(&player->lock);
      av_usleep(20 * 1000); // 20 ms
      continue;
    }

    if (!packet && player->vframe.width && player->vframe.height && *player->vframe.data) {
      render_video(player->render, &player->vframe); 
    } // 只有一帧的进行渲染，一般出现在mp3

    if (!(packet = pktqueue_video_dequeue(player))) { continue; }
    datarate_video_packet(player->datarate, packet);

    // avcodec_decode_video2 已经被丢弃，因为对于一个packet只能解码一帧
    while (packet && packet->size > 0 && !(player->status & (PS_V_PAUSE | PS_CLOSE))) {
      ret = decoder_decode_frame(player->vcodec_context, packet, &player->vframe, &got);
      if (ret < 0) {
        av_log(NULL, AV_LOG_WARNING, "an error occurred during decoding video. \n");
        break;
      }

      if (player->vcodec_context->width != player->init_params.video_vwidth || 
          player->vcodec_context->height != player->init_params.video_vheight) {
        player->init_params.video_vwidth = player->init_params.video_owidth = player->vcodec_context->width;
        player->init_params.video_vheight = player->init_params.video_oheight = player->vcodec_context->height;
        player_send_message(player->cmnvars.winmsg, MSG_VIDEO_RESIZED, NULL);
      }

      if (got) {
        // 是否加锁 
        player->vframe.height = player->vcodec_context->height;
        vfilter_graph_input(player, &player->vframe);
        do {
          if (vfilter_graph_output(player, &player->vframe) < 0) { break; }
          player->seek_vpts = player->vframe.best_effort_timestamp; // 读到的帧锁在pts
          // TODO: 应该不要转化
          player->vframe.pts = av_rescale_q(player->seek_vpts, player->vstream_timebase, TIMEBASE_MS);
          if (player->status & PS_V_SEEK) {
            if (player->seek_dest - player->vframe.pts <= player->seek_diff) {
              // TODO:
            }
          }
          // player->vframe.pts = player->seek_vpts;
          if (!(player->status & PS_V_SEEK)) { render_video(player->render, &player->vframe); }
        } while (player->vfilter_graph);
      } else {
        break;
      }
    }
    pktqueue_release_packet(player->pktqueue, packet); // 将帧放入队列中，这里到达了末尾
  }

#ifdef ANDROID
    JniDetachCurrentThread();
#endif
  return NULL;
}


static int init_stream(Player* player, enum AVMediaType type, int sel) {
  if (!player) { av_log(NULL, AV_LOG_WARNING, "player is null"); return -1; }

  const AVCodec* decoder = NULL;
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
          avcodec_parameters_to_context(player->acodec_context, player->avformat_context->streams[idx]->codecpar) == 0 && 
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

      player->vstream_timebase = player->avformat_context->streams[idx]->time_base;

      if (player->init_params.video_hwaccel) {
#ifdef ANDROID
        // ffmpeg能够调用android端的mediacode去进行编解码(GPU)
        // 查看[这里](https://trac.ffmpeg.org/wiki/HWAccelIntro)
        switch (player->vcodec_context->codec_id) {
          case AV_CODEC_ID_H264 : decoder = avcodec_find_encoder_by_name("h264_mediacodec"); break;
          case AV_CODEC_ID_HEVC : decoder = avcodec_find_encoder_by_name("hevc_mediacodec"); break;
          case AV_CODEC_ID_VP8 : decoder = avcodec_find_encoder_by_name("vp8_mediacodec"); break;
          case AV_CODEC_ID_VP9 : decoder = avcodec_find_encoder_by_name("vp9_mediacodec"); break;
          case AV_CODEC_ID_MPEG2VIDEO : decoder = avcodec_find_decoder_by_name("mpeg2_meidacodec"); break;
          case AV_CODEC_ID_MPEG4 : decoder = avcodec_find_decoder_by_name("mpeg4_mediacodec"); break;
          default: break;
        }

        if (decoder && 
            avcodec_parameters_copy(player->vcodec_context, player->avformat_context->streams[idex]->codecpar) == 0 && 
            avcodec_open2(player->vcodec_context, decoder, NULL) == 0) {
          player->vstream_index = idx;
          av_log(NULL, AV_LOG_WARNING, "using android mediacodec hardware decoder %s !\n", decoder->name);
        } else {
          decoder = NULL;
        }
        player->init_params.video_hwaccel = decoder ? 1 : 0;
#endif  
      }
      if (!decoder) {
        if (player->init_params.video_thread_count > 0) {
          player->vcodec_context->thread_count = player->init_params.video_thread_count;
        }
        decoder = avcodec_find_decoder(type);

        if (decoder && 
            avcodec_parameters_to_context(player->vcodec_context, player->avformat_context->streams[idx]->codecpar) == 0 && 
            avcodec_open2(player->vcodec_context, decoder, NULL) == 0) {
          player->vstream_index = idx;
        } else {
          av_log(NULL, AV_LOG_WARNING, "failed to open audio decoder");
        }
        player->init_params.video_thread_count = player->vcodec_context->thread_count;
      }
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      // 字幕流
      return -1;
    default:
      return -1;
  }
  return 0;
}

/*
 * @brief 解析输入数据，解析的数据数据为下面的格式: 
 *      [key1 =(空格,:) value1;(,) key2 =(空格,:) value2;(,) key3 =(空格,:) value3 ...]
 * @return 返回解析后的数据
 */ 
static char* parse_params(const char* str, const char* key, char* val, int len) {
  char* p = (char*)strstr(str, key);
  int i;
  if (!p) { return NULL; }
  p += strlen(key);
  if (*p == '\0') { return NULL; }

  while (1) {
    if (*p != ' ' && *p != '=' && *p != ':') { break; }
    else p++;
  }
  
  for (i = 0; i < len; i++) {
    if (*p == ',' || *p == ';' || *p == '\r' || 
        *p == '\n' || *p == '\0') { break; }
    else if (*p == '\\') { p++; }
    val[i] = *p++;
  } 
  val[i < len ? i : len - 1] = '\0';
  return val;
}

void* player_open(char* file, void* win, PlayerInitParams* params) {
  Player* player = (Player*)calloc(1, sizeof(Player));
  if (!player) { return NULL; }
  
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

  if (params) { memcpy(&player->init_params, params, sizeof(PlayerInitParams)); }
  player->cmnvars.init_params = &player->init_params;

  strcpy(player->url, file);

#ifdef ANDROID
  player->cmnvars.winmsg = JniRequestWinObj(win);
#endif

  pthread_create(&player->avdemux_thread, NULL, av_demux_thread_proc, player);

  pthread_create(&player->adecode_thread, NULL, audio_decode_thread_proc, player);
  pthread_create(&player->vdecode_thread, NULL, video_decode_thread_proc, player);

  return player;
error_handler:
  player_close(player);
  return NULL;
}

void player_close(void* ctxt) {
  Player* player = (Player*)ctxt;
  if (!player) { return; }

  player->read_timeout = 0;
  pthread_mutex_lock(&player->lock);
  player->status |= PS_CLOSE;
  pthread_mutex_unlock(&player->lock);
  render_pause(player->render, 2); // TODO: ?
  if (player->adecode_thread) { pthread_join(player->adecode_thread, NULL); }
  if (player->vdecode_thread) { pthread_join(player->vdecode_thread, NULL); }
  pthread_mutex_destroy(&player->lock);

  player_prepare_or_free(player, 0);
  // TODO: ?
  pktqueue_destroy(player->pktqueue);

#ifdef ANDROI
  JniReleaseWinObj(player->cmnvars.winmsg);
#endif

  free(player);

  avformat_network_deinit();
}



void player_load_params(PlayerInitParams* params, char* str) {
  char value[16];
  params->video_stream_cur = atoi(parse_params(str, "video_stream_cur", value, sizeof(value)) ? value : "0");
  params->video_thread_count = atoi(parse_params(str, "video_thread_count", value, sizeof(value)) ? value : "0");
  params->video_hwaccel = atoi(parse_params(str, "video_hwaccel", value, sizeof(value)) ? value : "0");
  params->video_deinterlace = atoi(parse_params(str, "video_deinterlace", value, sizeof(value)) ? value : "0");
  params->video_rotate = atoi(parse_params(str, "video_rotate", value, sizeof(value)) ? value : "0");
  params->video_bufpktn = atoi(parse_params(str, "video_bufpktn", value, sizeof(value)) ? value : "0");
  params->video_vwidth = atoi(parse_params(str, "video_vwidth", value, sizeof(value)) ? value : "0");
  params->video_vheight = atoi(parse_params(str, "video_vheight", value, sizeof(value)) ? value : "0");
  params->audio_stream_cur = atoi(parse_params(str, "audio_stream_cur", value, sizeof(value)) ? value : "0");
  params->audio_bufpktn = atoi(parse_params(str, "audio_bufpktn", value, sizeof(value)) ? value : "0");
  params->subtitle_stream_cur = atoi(parse_params(str, "subtitle_stream_cur", value, sizeof(value)) ? value : "0");
  params->vdev_render_type = atoi(parse_params(str, "vdev_render_type", value, sizeof(value)) ? value : "0");
  params->adev_render_type = atoi(parse_params(str, "adev_render_type", value, sizeof(value)) ? value : "0");
  params->adev_render_type = atoi(parse_params(str, "adev_render_type", value, sizeof(value)) ? value : "0");
  params->init_timeout = atoi(parse_params(str, "init_timeout", value, sizeof(value)) ? value : "0");
  params->open_autoplay = atoi(parse_params(str, "open_autoplay", value, sizeof(value)) ? value : "0");
  params->auto_reconnect = atoi(parse_params(str, "auto_reconnect", value, sizeof(value)) ? value : "0");
  params->rtsp_transport = atoi(parse_params(str, "rtsp_transport", value, sizeof(value)) ? value : "0");
  params->avts_syncmode = atoi(parse_params(str, "avts_syncmode", value, sizeof(value)) ? value : "0");
  params->swscale_type = atoi(parse_params(str, "swscale_type", value, sizeof(value)) ? value : "0");
  parse_params(str, "filter_string", params->filter_string, sizeof(params->filter_string));
  parse_params(str, "ffrdp_tx_key", params->ffrdp_tx_key, sizeof(params->ffrdp_tx_key));
  parse_params(str, "ffrdp_rx_key", params->ffrdp_rx_key, sizeof(params->ffrdp_rx_key));
}


// void player_setparam(void* hplayer, int id, void* param) {
  // if (!hplayer) { return; }
  // Player* player = (Player*)hplayer;
//
  // switch (id) {
// #ifdef ENABLE_FFRDP_SUPPORT
    // case PARAM_FFRDP_SENDDATA:
      // struct {
        // void* data;
        // uint32_t size;
      // }* data = param;
      // ffrdpdemuxer_senddata(player->ffrdpd, data->data, data->size); TODO:
      // break;
// #endif
    // default: return;
  // }
// }
