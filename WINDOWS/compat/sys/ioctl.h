/* Windows (MinGW64) shim for <sys/ioctl.h>: see win_compat.h for why.
 * FIONREAD/FIONBIO come from winsock2.h; the TIOCM_* modem-line bits are
 * emulated for serial ports (GetCommModemStatus/EscapeCommFunction) in
 * compat_ioctl(). */
#pragma once
#include "win_compat.h"
#include "termios.h"
