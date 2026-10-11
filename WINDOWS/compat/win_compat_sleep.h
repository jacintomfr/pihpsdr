/*
 * Included at the END of the winpthreads headers that declare or define
 * clock_nanosleep (see pthread_time.h and pthread.h in this folder), so
 * that the macro below applies to piHPSDR's code only.
 *
 * WHY: winpthreads' clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)
 * returns -1 at once without sleeping (measured: 500 x 2 ms absolute
 * sleeps took 0.000 s). piHPSDR paces several threads with it - e.g. the
 * client's TX audio thread, which then spun at full speed and sent the
 * microphone as rough audio, then as silence. compat_clock_nanosleep()
 * turns an absolute time into a relative sleep (win_compat.c).
 */
#ifdef _WIN32
#include <time.h>
int compat_clock_nanosleep(clockid_t clock_id, int flags,
                           const struct timespec *request, struct timespec *remain);
#undef clock_nanosleep
#define clock_nanosleep compat_clock_nanosleep   // also renames piHPSDR's own extern declarations
#endif
