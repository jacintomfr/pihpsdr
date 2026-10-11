/* Windows shim: <pthread.h> (it re-defines clock_nanosleep as a macro) */
#pragma once
#include_next <pthread.h>
#include "win_compat_sleep.h"
