#ifndef DDGPLAYER_STDEFINE_H_
#define DDGPLAYER_STDEFINE_H_

#define DO_USE_VAR(a) do { a = a; } while (0);

typedef struct {
  long left; 
  long top;
  long right;
  long bottom;
} Rect;

#define MAX(a, b) (a) > (b) ? (a) : (b)
#define MIN(a, b) (a) > (b) ? (b) : (a)

#endif
