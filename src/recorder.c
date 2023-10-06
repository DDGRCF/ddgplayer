#include "recorder.h"

#include <pthread.h>

typedef struct {
  AVFormatContext *ifc;
  AVFormatContext *ofc;
  pthread_mutex_t lock;
} Recorder;

void *recorder_init(char *filename, AVFormatContext *ifc) {
  Recorder *recorder;
  int ret, i;

  if (!filename || !ifc) {
    return NULL;
  }

  recorder = (Recorder *)calloc(1, sizeof(Recorder));
  if (!recorder) {
    return NULL;
  }

  recorder->ifc = ifc;

  // 直接可以从filename中解析
  avformat_alloc_output_context2(&recorder->ofc, NULL, NULL, filename);
  if (!recorder->ofc) {
    av_log(NULL, AV_LOG_ERROR,
           "failed to deduce output format from file extension");
    goto error_handler;
  }

  for (i = 0; i < (int)ifc->nb_streams; i++) { // TODO: Test
    AVStream *is = ifc->streams[i];
    AVStream *os = avformat_new_stream(
        recorder->ofc, NULL); // 后面参数估计为了兼容用的，没有使用
    if (!os) {
      av_log(NULL, AV_LOG_ERROR, "failed to allocating output stream !\n");
      goto error_handler;
    }

    ret = avcodec_parameters_copy(os->codecpar, is->codecpar);
    if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "failed to copy istream code parameters to ostream code "
             "parameters !\n");
      goto error_handler;
    }

    os->codecpar->codec_tag = 0; // TODO: ?
    // TODO: 好像老版本才出现这样的情况
    // if codec_tag != 0: 就根据这个来查找支持的封装格式，else 根据codec_id进行查找
    // 查看https://blog.csdn.net/yinshipin007/article/details/131090976?utm_medium=distribute.pc_relevant.none-task-blog-2~default~baidujs_baidulandingword~default-0-131090976-blog-75528653.235^v38^pc_relevant_sort_base3&spm=1001.2101.3001.4242.1&utm_relevant_index=3
    // os->codecpar->codec_tag = 0;
    // if (recorder->ofc->oformat->flags & AVFMT_GLOBALHEADER) {
    // }
  }

  // 网络url不需要打开
  if (!(recorder->ofc->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&recorder->ofc->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR, "failed to open %s !\n", filename);
      goto error_handler;
    }
  }

  ret = avformat_write_header(recorder->ofc, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to write header to output context !\n");
    goto error_handler;
  }

  pthread_mutex_init(&recorder->lock, NULL);
  return recorder;

error_handler:
  recorder_free(recorder);
  return NULL;
}

void recorder_free(void *ctxt) {
  Recorder *recorder = (Recorder *)ctxt;
  if (!ctxt) {
    return;
  }

  pthread_mutex_lock(&recorder->lock);

  if (recorder->ofc) {
    // 将output format还没有输出avpacket输出出来，然后添加流结束标志
    av_write_trailer(recorder->ofc);
    // 将打开的输出文件关闭
    if (!(recorder->ofc->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&recorder->ofc->pb);
    }
  }

  avformat_free_context(recorder->ofc);
  pthread_mutex_unlock(&recorder->lock);
  pthread_mutex_destroy(&recorder->lock);
  free(recorder);
}

int recorder_packet(void *ctxt, AVPacket *pkt) {
  Recorder *recorder = (Recorder *)ctxt;
  AVPacket packet = {0};
  AVStream *is, *os;
  int ret = 0;
  if (!ctxt || !pkt) {
    return -1;
  }

  pthread_mutex_lock(&recorder->lock);

  is = recorder->ifc->streams[pkt->stream_index];
  os = recorder->ofc->streams[pkt->stream_index];

  // 时间基转化
  ret = av_packet_ref(&packet, pkt);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to reference the packet !\n");
    return -1;
  }

  // TODO: ?
  packet.pts = av_rescale_q_rnd(packet.pts, is->time_base, os->time_base,
                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  packet.dts = av_rescale_q_rnd(packet.dts, is->time_base, os->time_base,
                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  packet.duration = av_rescale_q(packet.duration, is->time_base, os->time_base);
  packet.pos = -1;
  ret = av_interleaved_write_frame(recorder->ofc, &packet); // output
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "failed to write frame to output format !\n");
    return -1;
  }
  av_packet_unref(&packet);
  pthread_mutex_unlock(&recorder->lock);

  return 0;
}
