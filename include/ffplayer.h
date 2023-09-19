#ifndef DDGPLAYER_FFPLAYER_H_
#define DDGPLAYER_FFPLAYER_H_

#include <stdint.h>

#include <libavformat/avformat.h>

#define DDGPLAYER_VERSION "v1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

// fourc表示
#define MSG_OPEN_DONE      (('O' << 24) | ('P' << 16) | ('E' << 8) | ('N' << 0))
#define MSG_OPEN_FAILED    (('F' << 24) | ('A' << 16) | ('I' << 8) | ('L' << 0))
#define MSG_PLAY_COMPLETED (('E' << 24) | ('N' << 16) | ('D' << 8) | (' ' << 0))
#define MSG_TASK_SHAPSHOT  (('S' << 24) | ('N' << 16) | ('A' << 8) | ('P' << 0))
#define MSG_STREAM_CONNECTED \
  (('C' << 24) | ('N' << 16) | ('C' << 8) | ('T' << 0))
#define MSG_STREAM_DISCONNECT \
  (('D' << 24) | ('I' << 16) | ('S' << 8) | ('C' << 0))
#define MSG_VIDEO_RESIZED (('S' << 24) | ('I' << 16) | ('Z' << 8) | ('E' << 0))

enum {
  ADEV_RENDER_TYPE_WAVEOUT,
  ADEV_RENDER_TYPE_MAX_NUM,
};

enum {
  VDEV_RENDER_TYPE_MAX_NUM,
};

enum {
  VIDEO_MODE_LETTERBOX, // 长宽比不变
  VIDEO_MODE_STRETCHED,
  VIDEO_MODE_MAX_NUM,
};

enum {
  VISUAL_EFFECT_DISABLE,
  VISUAL_EFFECT_WAVEFORM,
  VISUAL_EFFECT_SPECTRUM,
  VISUAL_EFFECT_MAX_NUM,
};

enum {
  SEEK_STEP_FORWARD = 1,
  SEEK_STEP_BACKWARD,
};

enum {
  // duration & position
  PARAM_MEDIA_DURATION = 0x1000,
  PARAM_MEDIA_POSITION,

  // media detail info
  PARAM_VIDEO_WIDTH,
  PARAM_VIDEO_HEIGHT,

  // video display mode
  PARAM_VIDEO_MODE,

  // audio volume control
  PARAM_AUDIO_VOLUME,

  // playback speed control
  PARAM_PLAY_SPEED_VALUE,
  PARAM_PLAY_SPEED_TYPE,

  // visual effect mode
  PARAM_VISUAL_EFFECT,

  // audio/video sync diff
  PARAM_AVSYNC_TIME_DIFF,

  // get player init params
  PARAM_PLAYER_INIT_PARAMS,

  // definition evaluation
  PARAM_DEFINITION_VALUE,

  PARAM_DATARATE_VALUE,
  PARAM_OBJECT_DETECT,
  //-- public

  //++ for adev
  PARAM_ADEV_GET_CONTEXT = 0x2000,
  //-- for adev

  //++ for vdev
  PARAM_VDEV_GET_CONTEXT = 0x3000,
  PARAM_VDEV_POST_SURFACE,
  PARAM_VDEV_GET_D3DDEV,
  PARAM_VDEV_D3D_ROTATE,
  PARAM_VDEV_GET_OVERLAY_HDC,
  PARAM_VDEV_SET_OVERLAY_RECT,
  PARAM_VDEV_GET_VRECT,
  PARAM_VDEV_SET_BBOX,
  //-- for vdev

  //++ for render
  PARAM_RENDER_GET_CONTEXT = 0x4000,
  PARAM_RENDER_STEPFORWARD,
  PARAM_RENDER_VDEV_WIN,
  PARAM_RENDER_SOURCE_RECT,
  //-- for render
};

enum {
  AVSYNC_MODE_AUTO,       // 自动
  AVSYNC_MODE_FILE,       // 文件播放模式
  AVSYNC_MODE_LIVE_SYNC0, // 直播模式，放弃音视频同步
  AVSYNC_MODE_LIVE_SYNC1, // 直播模式，做音视频同步
};

/**
 * @brief 初始化参数
 */
typedef struct {
  int video_vwidth;       // wr 实际宽
  int video_vheight;      // wr 实际长
  int video_owidth;       // r 输出宽
  int video_oheight;      // r 输出长
  int video_frame_rate;   // wr 视频帧率
  int video_stream_totol; // r 视频总的帧数
  int video_stream_cur;   // wr 当前的视频流
  int video_thread_count; // wr 视频解码的线程数
  int video_hwaccel;      // wr TODO: ?
  int video_deinterlace;  // wr TODO: ?
  int video_rotate;       // wr 视频旋转角度
  int video_codec_id;     // wr 解码器id
  int video_bufpktn;      // wr 视频pkt缓冲区数量

  int audio_channels;     // r 音频通道数
  int audio_sample_rate;  // r 音采样率
  int audio_stream_total; // r 音频采样数
  int audio_stream_cur;   // wr 当前音频流
  int audio_bufpktn;      // wr 音频pkt缓冲数

  int subtitle_stream_total; // r 字幕流总数
  int subtitle_stream_cur;   // wr 当前字幕流

  int vdev_render_type; // w 视频渲染类型
  int adev_render_type; // w 音频渲染类型

  int init_timeout; // w 播放器初始化超时时间，用来防止卡死网络流媒体 ms
  int open_autoplay; // w 播放器打开后自动播放，不需要手动设置 MSG
  int auto_reconnect; // w 流媒体超时重连时间(毫秒)
  int rtsp_transport; // w rtsp传输模式，0 - 自动，1 - udp, 2 - tcp
  int avts_syncmode; // w 音视频时间戳同步模式， 0 - 自动，2 - 直播模式，3 - 直播模式
  char filter_string[256]; // w 自定义的video filter string(滤镜)

  char ffrdp_tx_key[32]; // w TODO: ?
  char ffrdp_rx_key[32]; // w TODO: ?
  int swscale_type;      // w ffrender图像swscale需要用到的类型
} PlayerInitParams;

typedef struct {
  PlayerInitParams *init_params;
  int64_t start_time; // ms
  int64_t start_tick; // TODO: ?
  int64_t start_pts;
  int64_t apts; // current apts
  int64_t vpts; // current vpts
  int apktn;    // available audio packet number in pktqueue
  int vpktn;    // available video packet number in pktqueue
  void *winmsg;
} CommonVars;

extern const int FF_TIME_MS;

extern const AVRational FF_TIME_BASE_Q;

extern const AVRational FF_FREQUENCY_Q;

void *player_open(char *file, void *win, PlayerInitParams *params);
void player_close(void *ctxt);
void player_play(void *hplayer);
void player_pause(void *hplayer);
void player_seek(void *hplayer, int64_t ms, int type);
void player_setrect(void *hplayer, int type, int x, int y, int w, int h);
int player_snapshot(void *hplayer, char *file, int w, int h, int wait_time);
int player_record(void *hplayer, char *file);
void player_setparam(void *hplayer, int id, void *param);
void player_getparam(void *hplayer, int id, void *param);

void *av_demux_thread_proc(void *ctxt);
void *audio_decode_thread_proc(void *ctxt);
void *video_decode_thread_proc(void *ctxt);

void player_send_message(void *extra, int32_t msg, void *param);
void player_load_params(PlayerInitParams *params, char *str);

#ifdef __cplusplus
}
#endif

#endif
