/* Windows (MinGW64) shim for <string.h>: adds the BSD index()/rindex()/
 * bzero()/bcopy() that glibc exposes through <string.h> but the MS CRT
 * lacks (used by vfo_menu.c, portaudio.c, client_thread.c). */
#pragma once
#include_next <string.h>
#ifndef index
  #define index(s,c)  strchr((s),(c))
  #define rindex(s,c) strrchr((s),(c))
#endif
#ifndef bzero
  #define bzero(p,n)    memset((p),0,(n))
  #define bcopy(s,d,n)  memmove((d),(s),(n))
#endif
