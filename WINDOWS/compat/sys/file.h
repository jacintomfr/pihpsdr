/* Windows (MinGW64) shim for <sys/file.h>: flock() is emulated with
 * LockFileEx() (single-instance lock in main.c). */
#pragma once
#include "win_compat_fd.h"
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#ifdef __cplusplus
extern "C" {
#endif
int flock(int fd, int operation);
#ifdef __cplusplus
}
#endif
