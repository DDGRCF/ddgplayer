#ifndef DDGPLAYER_FFRENDER_H_
#define DDGPLAYER_FFRENDER_H_
#include "ffplayer.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>


void* render_open(int adevtype, int vdevtype, void* surface, struct AVRational frate, 
                  int w, int h, CommonVars* cmnvars);
void render_close(void* hrender);
void render_audio(void* hrender, struct AVFrame* audio);
void render_video(void* hrender, struct AVFrame* video);
void render_setrect(void* hrender, int type, int x, int y, int w, int h);
void render_pause(void* hrender, int pause);
int render_snapshot(void* hplayer, char* file, int w, int h, int wait_time);
void render_setparam(void* hrender, int id, void* param);
void render_getparam(void* hrender, int id, void* param);


#ifdef __cplusplus
}
#endif



#endif
