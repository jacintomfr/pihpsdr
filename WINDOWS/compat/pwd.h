/* Windows (MinGW64) shim for <pwd.h>: getpwuid() only needs to deliver a
 * home directory (%USERPROFILE%) for startup.c. */
#pragma once
#include "win_compat_fd.h"
struct passwd {
  char *pw_name;
  char *pw_dir;
  char *pw_shell;
};
#ifdef __cplusplus
extern "C" {
#endif
struct passwd *getpwuid(unsigned int uid);
#ifdef __cplusplus
}
#endif
