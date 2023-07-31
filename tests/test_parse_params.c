#include "ffplayer.h"
#include <string.h>
#include <stdio.h>


static char* parse_params(const char* str, const char* key, char* val, int len) {
  char* p = (char*)strstr(str, key);
  int i;
  if (!p) { return NULL; }
  p += strlen(key);
  if (*p == '\0') { return NULL; }

  while (1) {
    if (*p != ' ' && *p != '=' && *p != ':') { break; }
    else p++;
  }
  
  for (i = 0; i < len; i++) {
    if (*p == ',' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\0') { break; }
    else if (*p == '\\') { p++; }
    val[i] = *p++;
  } 
  val[i < len ? i : len - 1] = '\0';
  return val;
}

int main() {
  char var[16];
  const char* str = "video = 0, audio 1, thread 3";
  parse_params(str, "audio", var, sizeof(var));
  printf("%s\n", var);
  return 0;
}
