/* Windows (MinGW64) shim for <poll.h>: Winsock provides WSAPoll() and
 * struct pollfd (sockets only). */
#pragma once
#include "win_compat.h"
typedef unsigned long nfds_t;
#define poll(f,n,t) WSAPoll((f),(ULONG)(n),(t))
