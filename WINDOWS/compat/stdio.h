/* Windows (MinGW64) shim for <stdio.h>.
 * WHY: POSIX rename() atomically replaces an existing target; the MS CRT's
 * rename() fails if the target exists. property.c saves every props file
 * as "write temp file, then rename() over the old one", so without this
 * no settings could ever be saved on Windows ("can't save ..."). */
#pragma once
#include_next <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
int compat_rename(const char *oldpath, const char *newpath);
#ifdef __cplusplus
}
#endif
#define rename(o,n) compat_rename((o),(n))
