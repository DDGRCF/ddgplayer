#ifndef DDGPLAYER_VEFFECT_H_
#define DDGPLAYER_VEFFECT_H_

#ifdef __cplusplus
extern "C" {
#endif

void *veffect_create(void *surface);
void veffect_destroy(void *ctxt);
void veffect_render(void *ctxt, int x, int y, int w, int h, int type,
                    void *adev);

#ifdef __cplusplus
}
#endif

#endif
