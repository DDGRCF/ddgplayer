#ifndef DDGPLAYER_FFPLAYER_H_
#define DDGPLAYER_FFPLAYER_H_

#include <stdint.h>
/* 
 * @brief 初始化参数
 */
typedef struct {
  int video_vwidth; // wr 实际宽
  int video_vheight; // wr 实际长
  int video_owidth; // r 输出宽
  int video_oheight; // r 输出长
  int video_frame_rate; // wr 视频帧率
  int video_stream_totol; // r 视频总的帧数
  int video_stream_cur; // wr 当前的视频流
  int video_thread_count; // wr 视频解码的线程数
  int video_hwaccel; // wr TODO: ?
  int video_deinterlace; // wr TODO: ?
  int video_rotate;  // wr 视频旋转角度
  int video_codec_id; // wr 解码器id
  int video_bufpktn; // wr 视频pkt缓冲区数量

  int audio_channels; // r 音频通道数
  int audio_sample_rate; // r 音采样率
  int audio_stream_total;  // r 音频采样数
  int audio_stream_cur; // wr 当前音频流 
  int audio_bufpktn; // wr 音频pkt缓冲数

  int subtitle_stream_total; // r 字幕流总数
  int subtitle_stream_cur; // wr 当前字幕流

  int vdev_render_type; // w 视频渲染类型
  int adev_render_type; // w 音频渲染类型

  int init_timeout; // w 播放器初始化超时时间，用来防止卡死网络流媒体
  int open_autoplay; // w 播放器打开后自动播放，不需要手动设置 MSG
  int auto_reconnect; // w 流媒体超时重连时间
  int rtsp_transport; // w rtsp传输模式，0 - 自动，1 - udp, 2 - tcp
  int avts_syncmode; // w 音视频时间戳同步模式， 0 - 自动，2 - 直播模式，3 - 直播模式
  char filter_string[256]; // w 自定义的video filter string(滤镜)

  char ffrdp_tx_key[32]; // w TODO: ?
  char ffrdp_rx_key[32]; // w TODO: ?
  int swscale_type; // w ffrender图像swscale需要用到的类型
} PlayerInitParams;


typedef struct {
  PlayerInitParams* init_params;
  int64_t start_time;
  int64_t start_tick; // TODO: ?
  int64_t start_pts;
  int64_t apts; // current apts
  int64_t vpts; // current vpts
  int apktn; // available audio packet number in pktqueue
  int vkptn; // available video packet number in pktqueue
  void* winmsg;
} CommonVars;

#endif
