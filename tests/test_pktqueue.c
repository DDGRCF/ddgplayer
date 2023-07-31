#include <pktqueue.h>

#include <stdio.h>

int main() {
  void* ppq = pktqueue_create(10, NULL);
  pktqueue_destroy(ppq);
  return 0;
}
