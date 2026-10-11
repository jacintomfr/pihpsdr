/* Windows shim: <pthread_time.h> with a working clock_nanosleep() */
#pragma once
#include_next <pthread_time.h>
#include "win_compat_sleep.h"
