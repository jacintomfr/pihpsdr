/* Windows (MinGW64) shim for <sys/utsname.h>: uname() is filled from
 * RtlGetVersion()/GetComputerName() in win_compat.c (used for the log and
 * the About dialog). */
#pragma once
#define _UTSNAME_LENGTH 65
struct utsname {
  char sysname[_UTSNAME_LENGTH];
  char nodename[_UTSNAME_LENGTH];
  char release[_UTSNAME_LENGTH];
  char version[_UTSNAME_LENGTH];
  char machine[_UTSNAME_LENGTH];
};
#ifdef __cplusplus
extern "C" {
#endif
int uname(struct utsname *buf);
#ifdef __cplusplus
}
#endif
