/* Windows (MinGW64) shim for <sys/types.h>.
 * WHY: glibc's <sys/types.h> also defines pthread_t, pthread_mutex_t, ...
 * and several piHPSDR headers (main.h, the protocol files) rely on that
 * instead of including <pthread.h>. Mirror that behaviour. */
#pragma once
#include_next <sys/types.h>
#include <pthread.h>
