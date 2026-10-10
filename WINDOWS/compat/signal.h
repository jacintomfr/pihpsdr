/* Windows (MinGW64) shim for <signal.h>.
 * WHY: there is no SIGPIPE on Windows (a write to a closed socket just
 * returns an error). tci.c does signal(SIGPIPE, SIG_IGN); passing an
 * unknown signal number to the MS CRT's signal() may trigger its
 * invalid-parameter handler, so that request is answered here instead. */
#pragma once
#include_next <signal.h>
#include "win_compat_fd.h"

#ifndef SIGPIPE
  #define SIGPIPE 13
#endif

static inline __p_sig_fn_t compat_signal(int sig, __p_sig_fn_t handler) {
  if (sig == SIGPIPE) { return SIG_DFL; }   /* nothing to ignore on Windows */
  return signal(sig, handler);
}
#define signal(s,h) compat_signal((s),(h))
