#ifndef DDGPLAYER_PKTQUEUE_H_
#define DDGPLAYER_PKTQUEUE_H_

#include "ffplayer.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "libavformat/avformat.h"

void *pktqueue_create(int size, CommonVars *cmnvars);
void pktqueue_destroy(void *ctxt);
void pktqueue_reset(void *ctxt);

AVPacket *pktqueue_request_packet(void *ctxt);
void pktqueue_release_packet(void *ctxt, AVPacket *pkt);

void pktqueue_audio_enqueue(void *ctxt, AVPacket *pkt);
AVPacket *pktqueue_audio_dequeue(void *ctxt);

void pktqueue_video_enqueue(void *ctxt, AVPacket *pkt);
AVPacket *pktqueue_video_dequeue(void *ctxt);

#ifdef __cplusplus
}
#endif

#endif
