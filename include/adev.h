#ifndef DDGPLAYER_ADEV_H_
#define DDGPLAYER_ADEV_H_ 

#include <stdint.h>

#include "ffplayer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADEV_SAMPLE_RATE 48000
#define ADEV_CLOSE     (1 << 0)
#define ADEV_COMPLETED (1 << 1)
#define ADEV_CLEAR     (1 << 2)

#define ADEV_COMMON_MEMBERS \
    int64_t* ppts;         \
    int16_t* bufcur;       \
    int bufnum;            \
    int buflen;            \
    int head;              \
    int tail;              \
    int status;            \
    CommonVars cmnvars

typedef struct {
  ADEV_COMMON_MEMBERS; 
} AdevCommonContext;

void* adev_create(int type, int bufnum, int buflen, CommonVars* cmnvars);
void adev_destroy(void* ctxt);
void adev_write(void* ctxt, uint8_t* buf, int len, int64_t pts);
void adev_setparam(void* ctxt, int id, void* param);
void adev_getparam(void* ctxt, int id, void* param);

#ifdef __cplusplus
}
#endif

#endif
