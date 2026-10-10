/* Windows (MinGW64) shim for <sys/mman.h>: included (but not used) by
 * iambic.c. Memory locking is a no-op on Windows. */
#pragma once
#define MCL_CURRENT 1
#define MCL_FUTURE  2
static inline int mlockall(int flags) { (void) flags; return 0; }
static inline int munlockall(void) { return 0; }
