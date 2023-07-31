#include "pktqueue.h"

#include <pthread.h>
#include <time.h>

#ifndef DEF_PKT_QUEUE_SIZE
#define DEF_PKT_QUEUE_SIZE 256
#endif


typedef struct {
  int fsize; // TODO:
  int asize;
  int vsize;
  int fncur; // 数量
  int ancur;
  int vncur;
  int fhead; // 第一个元素
  int ftail; // 最后一个元素的后一个
  int ahead;
  int atail;
  int vhead;
  int vtail;

  #define TS_STOP (1 << 0)
  #define TS_START (1 << 1)

  int status;

  AVPacket* bpkts; // packet buffers
  AVPacket** fpkts;
  AVPacket** apkts;
  AVPacket** vpkts;
  CommonVars* cmnvars;

  pthread_mutex_t lock;
  pthread_cond_t cond;
} PktQueue;

void* pktqueue_create(int size, CommonVars* cmnvars) {
  PktQueue* ppq; int i;

  size = size ? size : DEF_PKT_QUEUE_SIZE;
  // 结构体长度 + 结构体里面的数组长度
  ppq = (PktQueue*)calloc(1, 
      sizeof(PktQueue) + size * sizeof(AVPacket) + 3 * size * sizeof(AVPacket));
  if (!ppq) {
    av_log(NULL, AV_LOG_ERROR, "failed to allocated pktqueue context : !\n");
    exit(-1);
  }

  ppq->fncur = ppq->asize = ppq->vsize = ppq->fsize = size;
  ppq->bpkts = (AVPacket*)((uint8_t*)ppq + sizeof(PktQueue));
  ppq->fpkts = (AVPacket**)((uint8_t*)ppq->bpkts + sizeof(AVPacket) * size);
  ppq->apkts = (AVPacket**)((uint8_t*)ppq->fpkts + sizeof(AVPacket*) * size);
  ppq->vpkts = (AVPacket**)((uint8_t*)ppq->apkts + sizeof(AVPacket*) * size);
  ppq->cmnvars = cmnvars; // 是否要重新分配
  ppq->status = TS_START;
  pthread_mutex_init(&ppq->lock, NULL);
  pthread_cond_init(&ppq->cond, NULL);

  for (i = 0; i < ppq->fsize; i++) {
    ppq->fpkts[i] = &ppq->bpkts[i];
  }

  return ppq;
}

void pktqueue_destroy(void* ctxt) {
  PktQueue* ppq = (PktQueue*)ctxt;
  int i;

  for (i = 0; i < ppq->fsize; i++) {
    av_packet_unref(&ppq->bpkts[i]);
  }

  pthread_mutex_destroy(&ppq->lock);
  pthread_cond_destroy(&ppq->cond);

  free(ppq);
}


void pktqueue_reset(void* ctxt) {
  PktQueue* ppq = (PktQueue*)ctxt;
  int i;
  pthread_mutex_lock(&ppq->lock);
  for (i = 0; i < ppq->fsize; i++) {
    ppq->fpkts[i] = &ppq->bpkts[i];
    ppq->apkts[i] = NULL;
    ppq->vpkts[i] = NULL;
  }
  // TODO: ?
  ppq->fncur = ppq->asize;
  ppq->ancur = ppq->vncur = 0;
  ppq->fhead = ppq->ftail = 0;
  ppq->ahead = ppq->atail = 0;
  ppq->vhead = ppq->vtail = 0;

  // 解锁问题，参考https://juejin.cn/post/7101138173748576292
  pthread_cond_signal(&ppq->cond);
  pthread_mutex_unlock(&ppq->lock);
}


// 从队列里面拿出帧
AVPacket* pktqueue_request_packet(void* ctxt) {
  PktQueue* ppq = (PktQueue*)ctxt;
  AVPacket* pkt = NULL;
  struct timespec ts;
  int ret = 0;

  ret = clock_gettime(CLOCK_REALTIME, &ts);
  if (ret == -1) {
    av_log(NULL, AV_LOG_ERROR, "failed to get current time!\n");
    exit(-1);
  }
  // 过期时间是当前时间 + 0.1秒
  ts.tv_nsec += 100 * 1000 * 100;
  ts.tv_sec += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;

  pthread_mutex_lock(&ppq->lock);
  while (ppq->fncur == 0 && (ppq->status & TS_STOP) == 0 && ret != ETIMEDOUT) {
    ret = pthread_cond_timedwait(&ppq->cond, &ppq->lock, &ts);
  }
  if (ppq->fncur != 0) {
    ppq->fncur--;
    pkt = ppq->fpkts[ppq->fhead++ & (ppq->fsize - 1)];
    av_packet_unref(pkt);
    pthread_cond_signal(&ppq->cond);
  } 

  pthread_mutex_unlock(&ppq->lock);
  return pkt;
}

// 把帧放进队列中(timeout，最大等待时间)
void pktqueue_release_packet(void* ctxt, AVPacket* pkt) {
  PktQueue* ppq = (PktQueue*)ctxt;
  struct timespec ts;
  int ret = 0;
  ret = clock_gettime(CLOCK_REALTIME, &ts);
  if (ret == -1) {
    av_log(NULL, AV_LOG_ERROR, "failed to get current time!\n");
    exit(-1);
  }
  // 过期时间是当前时间 + 0.1秒
  ts.tv_nsec += 100 * 1000 * 100;
  ts.tv_sec += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;

  pthread_mutex_lock(&ppq->lock);
  while (ppq->fncur == ppq->fsize && (ppq->status & TS_STOP) == 0 && ret != ETIMEDOUT) {
    ret = pthread_cond_timedwait(&ppq->cond, &ppq->lock, &ts);
  }
  if (ppq->fncur != ppq->fsize) {
    ppq->fncur++;
    pkt = ppq->fpkts[ppq->ftail++ & (ppq->fsize - 1)] = pkt;
    pthread_cond_signal(&ppq->cond);
  } 

  pthread_mutex_unlock(&ppq->lock);
}
