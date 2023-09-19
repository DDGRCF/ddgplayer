#ifndef DDGPLAYER_DATARATE_H_
#define DDGPLAYER_DATARATE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>

void *datarate_create();

void datarate_destroy(void *ctxt);

void datarate_reset(void *ctxt);

void datarate_result(void *ctxt, int *arate, int *vrate, int *drate);

void datarate_audio_packet(void *ctxt, AVPacket *pkt);

void datarate_video_packet(void *ctxt, AVPacket *pkt);

#ifdef __cplusplus
}
#endif

#endif
