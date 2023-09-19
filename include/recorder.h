#ifndef DDGPLAYER_RECORDER_H_
#define DDGPLAYER_RECORDER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

void *recorder_init(char *filename, AVFormatContext *ifc);

void recorder_free(void *ctxt);

int recorder_packet(void *ctxt, AVPacket *pkt);

#ifdef __cplusplus
}
#endif

#endif
