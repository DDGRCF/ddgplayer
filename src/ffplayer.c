#include "ffplayer.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

// thread status
#define PS_A_PAUSE (1 << 0)
#define PS_V_PAUSE (1 << 1)
#define PS_R_PAUSE (1 << 2)
#define PS_F_SEEK (1 << 3)
#define PS_A_SEEK (1 << 4)
#define PS_V_SEEK (1 << 5)
#define PS_RECONNECT (1 << 6)
#define PS_CLOSE (1 << 7)

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

  int status;

   // player init timeout, and init params
  int64_t read_timelast; // 上一次读取的时间，主要用于音视频同步
  int64_t read_timeout; // 读取是否超时

  PlayerInitParams init_params;

} Player;

// static const AVRational TIMEBASE_MS = {1, 1000};

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
  int total = 0;
  for (int i = 0; i < (int)player->avformat_context->nb_streams; i++) {
    if (player->avformat_context->streams[i]->codecpar->codec_type == type) {
      total++;
    }
  }
  return total;
}

int init_stream(Player* player, enum AVMediaType type, int sel) {
  if (!player) { av_log(NULL, AV_LOG_WARNING, "player is null"); return -1; }

  const AVCodec* decoder = NULL;
  int idx = -1, cur = -1;

  for (int i = 0; i < (int)player->avformat_context->nb_streams; i++) {
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

