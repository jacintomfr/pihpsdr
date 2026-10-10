/* Windows (MinGW64) shim for <sys/resource.h>.
 * getrusage(RUSAGE_SELF) is implemented with GetProcessTimes() (CPU load
 * display); get/setpriority() map to the Win32 process priority class. */
#pragma once
#include <sys/time.h>
#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1
#define PRIO_PROCESS 0
struct rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_maxrss;
};
#ifdef __cplusplus
extern "C" {
#endif
int getrusage(int who, struct rusage *usage);
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);
#ifdef __cplusplus
}
#endif
