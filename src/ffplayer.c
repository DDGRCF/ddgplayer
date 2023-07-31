#include "ffplayer.h"

#include <pthread.h>
#include <string.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

#include "stdefine.h"


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
  void* pktqueue;
  void* render;

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

   // player init timeout, and init params
  int64_t read_timelast; // 上一次读取的时间，主要用于音视频同步
  int64_t read_timeout; // 读取是否超时

  PlayerInitParams init_params;

  // player common vars
  pthread_mutex_t lock;
  pthread_t avdemux_thread;
  pthread_t adecode_thread;
  pthread_t vdecode_thread;

} Player;


static void avlog_callback(void* ptr, int level, const char* fmt,  va_list vl) {
  DO_USE_VAR(ptr);  // TODO: ?
  if (level <= av_log_get_level()) {
#ifdef ANDROID
    __android_log_vprint(ANDROID_LOG_DEBUG, "fanplayer", fmt, vl);
#endif
  }
}

/**
 * @brief  中断的回调函数
 * @return 产生错误
**/
static int interrupt_callback(void* param) {
  Player* player = (Player*)param;
  if (player->read_timeout == -1) {
    return 0;
  }
  return av_gettime_relative() - player->read_timeout > player->read_timeout
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
  

  return NULL;
}

int init_stream(Player* player, enum AVMediaType type, int sel) {
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
