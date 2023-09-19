#include "datarate.h"

#include <libavutil/time.h>

typedef struct {
  uint64_t tick_start;
  uint32_t audio_bytes;
  uint32_t video_bytes;
} DataRate;

void *datarate_create() {
  void *ctxt = calloc(1, sizeof(DataRate));
  datarate_reset(ctxt);
  return ctxt;
}

void datarate_destroy(void *ctxt) {
  if (ctxt) {
    free(ctxt);
  }
}

void datarate_reset(void *ctxt) {
  DataRate *datarate = (DataRate *)ctxt;
  if (datarate) {
    datarate->tick_start = av_gettime_relative();
    datarate->audio_bytes = datarate->video_bytes = 0;
  }
}

void datarate_result(void *ctxt, int *arate, int *vrate, int *drate) {
  DataRate *datarate = (DataRate *)ctxt;
  if (datarate) {
    uint64_t tickcur = av_gettime_relative();
    int64_t tickdiff = (int64_t)tickcur - (int64_t)datarate->tick_start;
    if (tickdiff == 0)
      tickdiff = 1;
    if (arate) {
      *arate = (int)(datarate->audio_bytes * 1000000.0 / tickdiff);
    } // b/s
    if (vrate) {
      *vrate = (int)(datarate->video_bytes * 1000000.0 / tickdiff);
    } // b/s
    if (drate) {
      *drate = (int)((datarate->audio_bytes + datarate->video_bytes) *
                     1000000.0 / tickdiff);
    } // b/s
    datarate->tick_start +=
        tickdiff / 2; // 这里除以2的目的是，每次都只取前一次一般的byte进行计算
        // 这样bytes率就不是整体的，而是局部的
    datarate->audio_bytes /= 2;
    datarate->video_bytes /= 2;
  }
}

void datarate_audio_packet(void *ctxt, AVPacket *pkt) {
  DataRate *datarate = (DataRate *)ctxt;
  if (datarate) {
    datarate->audio_bytes += pkt->size;
  }
}

void datarate_video_packet(void *ctxt, AVPacket *pkt) {
  DataRate *datarate = (DataRate *)ctxt;
  if (datarate) {
    datarate->video_bytes += pkt->size;
  }
}
