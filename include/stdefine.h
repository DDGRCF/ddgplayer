#ifndef DDGPLAYER_STDEFINE_H_
#define DDGPLAYER_STDEFINE_H_

typedef struct {
  long left;
  long top;
  long right;
  long bottom;
} Rect;

#ifdef ANDROID
#include <android/log.h>

#define CONFIG_ENABLE_VEFFECT    0
#define CONFIG_ENABLE_SNAPSHOT   1
#define CONFIG_ENABLE_SOUNDTOUCH 0 // TODO(ddgrcf): to enable soundtouch
#define TCHAR                    cahr

#endif

#define MAX(a, b) (a) > (b) ? (a) : (b)
#define MIN(a, b) (a) > (b) ? (b) : (a)
#define DO_USE_VAR(a) \
  do {                \
    a = a;            \
  } while (0);

#endif
