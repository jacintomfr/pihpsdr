/* Windows (MinGW64) shim for <termios.h>.
 *
 * WHY: piHPSDR's CAT and serial PTT/TUNE code is written with termios.
 * On Windows, a serial port is a COMx HANDLE configured through a DCB and
 * COMMTIMEOUTS. compat_open() opens "COMx" (also accepts "\.\COMx" and
 * "/dev/ttySx" -> COM(x+1)), and the functions below translate the termios
 * settings piHPSDR actually uses (baud, 8N1/parity, VMIN/VTIME, RTS/DTR)
 * to the Win32 comm API (implemented in win_compat.c).
 *
 * Baud-rate constants are the plain numeric rates (B9600 == 9600), which
 * makes cfsetispeed() trivial on Windows.
 */
#pragma once
#include "win_compat_fd.h"

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32
struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t     c_cc[NCCS];
  speed_t  c_ispeed;
  speed_t  c_ospeed;
};

/* c_cc indices */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000

/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004

/* c_cflag */
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define CRTSCTS 020000000000

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define IEXTEN  0100000

/* tcsetattr / tcflush actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* baud rates: numeric on Windows */
#define B0      0
#define B300    300
#define B1200   1200
#define B2400   2400
#define B4800   4800
#define B9600   9600
#define B19200  19200
#define B38400  38400
#define B57600  57600
#define B115200 115200
#define B230400 230400
#define B460800 460800

/* modem lines (TIOCMGET/TIOCMSET/TIOCMBIS/TIOCMBIC) */
#define TIOCM_LE  0x001
#define TIOCM_DTR 0x002
#define TIOCM_RTS 0x004
#define TIOCM_ST  0x008
#define TIOCM_SR  0x010
#define TIOCM_CTS 0x020
#define TIOCM_CAR 0x040
#define TIOCM_CD  TIOCM_CAR
#define TIOCM_RNG 0x080
#define TIOCM_RI  TIOCM_RNG
#define TIOCM_DSR 0x100
#define TIOCMGET  0x5415
#define TIOCMBIS  0x5416
#define TIOCMBIC  0x5417
#define TIOCMSET  0x5418

#ifdef __cplusplus
extern "C" {
#endif
int     tcgetattr(int fd, struct termios *t);
int     tcsetattr(int fd, int action, const struct termios *t);
int     tcflush(int fd, int queue);
int     tcdrain(int fd);
int     cfsetispeed(struct termios *t, speed_t s);
int     cfsetospeed(struct termios *t, speed_t s);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
void    cfmakeraw(struct termios *t);
#ifdef __cplusplus
}
#endif
