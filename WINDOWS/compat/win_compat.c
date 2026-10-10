/* Copyright (C)
* 2026 - piHPSDR Windows (MSYS2 MinGW64) port
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*/

/*
 * Implementation of the Windows POSIX compatibility layer.
 * See win_compat.h for the rationale behind each wrapper.
 */

#ifdef _WIN32

#include "win_compat.h"
#include "termios.h"
#include "sys/utsname.h"
#include "sys/resource.h"
#include "sys/file.h"
#include "pwd.h"

/* This file implements the wrappers, so it must call the real functions. */
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef recv
#undef recvfrom
#undef send
#undef sendto
#undef setsockopt
#undef getsockopt
#undef shutdown
#undef select
#undef getsockname
#undef getpeername
#undef open
#undef close
#undef read
#undef write
#undef fcntl
#undef ioctl
#undef mkdir
#undef kill
#undef getuid
#undef geteuid
#undef rename

#include <iphlpapi.h>
#include <mstcpip.h>
#include <mmsystem.h>
#include <direct.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* ------------------------------------------------------------------ */
/* start-up: Winsock and 1 ms timer resolution                        */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION compat_lock;

static void compat_exit(void) {
  timeEndPeriod(1);
  WSACleanup();
}

/*
 * WHY a constructor: on Windows no socket call works before WSAStartup().
 * On POSIX there is no such step, so piHPSDR has no place for it; running
 * it before main() keeps main.c untouched.
 * timeBeginPeriod(1): the default Windows scheduler tick is 15.6 ms, which
 * would make every usleep()/clock_nanosleep() in the protocol and CW
 * threads (1..10 ms waits) wildly inaccurate.
 */
__attribute__((constructor))
static void compat_init(void) {
  WSADATA wsa;
  InitializeCriticalSection(&compat_lock);
  /*
   * POSIX has no text mode. Without this, every fopen(..., "w") (props
   * files, logs) would write CR/LF line ends. This also keeps the props
   * files byte-identical to the ones of the Linux/macOS versions.
   */
  _set_fmode(_O_BINARY);
  WSAStartup(MAKEWORD(2, 2), &wsa);
  timeBeginPeriod(1);
  atexit(compat_exit);
}

/* ------------------------------------------------------------------ */
/* errno mapping                                                      */
/* ------------------------------------------------------------------ */

static int wsa_to_errno(int wsa) {
  switch (wsa) {
  case WSAEWOULDBLOCK:     return EAGAIN;   /* piHPSDR often checks EAGAIN only */
  case WSAETIMEDOUT:       return EAGAIN;   /* SO_RCVTIMEO expiry is EAGAIN on POSIX */
  case WSAEINPROGRESS:     return EINPROGRESS;
  case WSAEALREADY:        return EALREADY;
  case WSAEINTR:           return EINTR;
  case WSAEBADF:           return EBADF;
  case WSAENOTSOCK:        return ENOTSOCK;
  case WSAEACCES:          return EACCES;
  case WSAEFAULT:          return EFAULT;
  case WSAEINVAL:          return EINVAL;
  case WSAEMFILE:          return EMFILE;
  case WSAEMSGSIZE:        return EMSGSIZE;
  case WSAEAFNOSUPPORT:    return EAFNOSUPPORT;
  case WSAEADDRINUSE:      return EADDRINUSE;
  case WSAEADDRNOTAVAIL:   return EADDRNOTAVAIL;
  case WSAENETDOWN:        return ENETDOWN;
  case WSAENETUNREACH:     return ENETUNREACH;
  case WSAENETRESET:       return ENETRESET;
  case WSAECONNABORTED:    return ECONNABORTED;
  case WSAECONNRESET:      return ECONNRESET;
  case WSAENOBUFS:         return ENOBUFS;
  case WSAEISCONN:         return EISCONN;
  case WSAENOTCONN:        return ENOTCONN;
  case WSAESHUTDOWN:       return EPIPE;
  case WSAECONNREFUSED:    return ECONNREFUSED;
  case WSAEHOSTUNREACH:    return EHOSTUNREACH;
  case WSAEOPNOTSUPP:      return EOPNOTSUPP;
  default:                 return EIO;
  }
}

static int set_errno_from_wsa(void) {
  errno = wsa_to_errno(WSAGetLastError());
  return -1;
}

/* ------------------------------------------------------------------ */
/* fd bookkeeping: serial ports and socket O_NONBLOCK flags           */
/* ------------------------------------------------------------------ */

#define MAX_SERIAL_FDS 16
typedef struct {
  int    used;
  int    fd;          /* CRT fd returned to piHPSDR */
  HANDLE h;
  int    nonblock;
  int    vmin;
  int    vtime;       /* 1/10 s */
  int    dtr;         /* current output line states (tracked, Win32 cannot read them back) */
  int    rts;
} serial_t;

static serial_t serials[MAX_SERIAL_FDS];

static serial_t *serial_find(int fd) {
  serial_t *s = NULL;
  EnterCriticalSection(&compat_lock);
  for (int i = 0; i < MAX_SERIAL_FDS; i++) {
    if (serials[i].used && serials[i].fd == fd) {
      s = &serials[i];
      break;
    }
  }
  LeaveCriticalSection(&compat_lock);
  return s;
}

#define MAX_NB_SOCKS 256
static int nb_socks[MAX_NB_SOCKS];     /* sockets currently in non-blocking mode */

static int sock_nonblock_get(int s) {
  int r = 0;
  EnterCriticalSection(&compat_lock);
  for (int i = 0; i < MAX_NB_SOCKS; i++) {
    if (nb_socks[i] == s) { r = 1; break; }
  }
  LeaveCriticalSection(&compat_lock);
  return r;
}

static void sock_nonblock_set(int s, int on) {
  EnterCriticalSection(&compat_lock);
  int free_slot = -1;
  for (int i = 0; i < MAX_NB_SOCKS; i++) {
    if (nb_socks[i] == s) {
      if (!on) { nb_socks[i] = 0; }
      LeaveCriticalSection(&compat_lock);
      return;
    }
    if (nb_socks[i] == 0 && free_slot < 0) { free_slot = i; }
  }
  if (on && free_slot >= 0) { nb_socks[free_slot] = s; }
  LeaveCriticalSection(&compat_lock);
}

static int is_socket(int fd) {
  int type;
  int len = sizeof(type);
  if (fd <= 0) { return 0; }
  return getsockopt((SOCKET) fd, SOL_SOCKET, SO_TYPE, (char *) &type, &len) == 0;
}

/* ------------------------------------------------------------------ */
/* sockets                                                            */
/* ------------------------------------------------------------------ */

int compat_socket(int domain, int type, int protocol) {
  SOCKET s = socket(domain, type, protocol);
  if (s == INVALID_SOCKET) { return set_errno_from_wsa(); }
  if (type == SOCK_DGRAM) {
    /*
     * ROOT CAUSE FIX (Windows only): an ICMP "port unreachable" (e.g. the
     * radio was switched off, or a discovery reply to a closed port) makes
     * the *next* recvfrom() on a Windows UDP socket fail with WSAECONNRESET.
     * POSIX never does that, and piHPSDR's receive threads would treat it
     * as fatal. Switch this Windows peculiarity off.
     */
    BOOL off = FALSE;
    DWORD dummy = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &dummy, NULL, NULL);
  }
  return (int) s;
}

int compat_connect(int s, const struct sockaddr *name, int namelen) {
  if (connect((SOCKET) s, name, namelen) == SOCKET_ERROR) {
    int e = WSAGetLastError();
    /* a non-blocking connect reports WSAEWOULDBLOCK where POSIX says EINPROGRESS */
    errno = (e == WSAEWOULDBLOCK) ? EINPROGRESS : wsa_to_errno(e);
    return -1;
  }
  return 0;
}

int compat_bind(int s, const struct sockaddr *name, int namelen) {
  return bind((SOCKET) s, name, namelen) == SOCKET_ERROR ? set_errno_from_wsa() : 0;
}

int compat_listen(int s, int backlog) {
  return listen((SOCKET) s, backlog) == SOCKET_ERROR ? set_errno_from_wsa() : 0;
}

int compat_accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
  SOCKET r = accept((SOCKET) s, addr, addrlen);
  if (r == INVALID_SOCKET) { return set_errno_from_wsa(); }
  return (int) r;
}

/* MSG_DONTWAIT does not exist on Windows: emulate with a zero-timeout select() */
static int would_block(int s, int for_write) {
  fd_set fds;
  struct timeval tv = {0, 0};
  FD_ZERO(&fds);
  FD_SET((SOCKET) s, &fds);
  int rc = select(0, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
  return rc == 0;
}

ssize_t compat_recv(int s, void *buf, size_t len, int flags) {
  if (flags & MSG_DONTWAIT) {
    flags &= ~MSG_DONTWAIT;
    if (would_block(s, 0)) { errno = EAGAIN; return -1; }
  }
  int rc = recv((SOCKET) s, (char *) buf, (int) len, flags);
  if (rc == SOCKET_ERROR) {
    /* POSIX silently truncates an over-long datagram */
    if (WSAGetLastError() == WSAEMSGSIZE) { return (ssize_t) len; }
    return set_errno_from_wsa();
  }
  return rc;
}

ssize_t compat_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
  if (flags & MSG_DONTWAIT) {
    flags &= ~MSG_DONTWAIT;
    if (would_block(s, 0)) { errno = EAGAIN; return -1; }
  }
  int rc = recvfrom((SOCKET) s, (char *) buf, (int) len, flags, from, fromlen);
  if (rc == SOCKET_ERROR) {
    if (WSAGetLastError() == WSAEMSGSIZE) { return (ssize_t) len; }
    return set_errno_from_wsa();
  }
  return rc;
}

ssize_t compat_send(int s, const void *buf, size_t len, int flags) {
  if (flags & MSG_DONTWAIT) {
    flags &= ~MSG_DONTWAIT;
    if (would_block(s, 1)) { errno = EAGAIN; return -1; }
  }
  int rc = send((SOCKET) s, (const char *) buf, (int) len, flags);
  return rc == SOCKET_ERROR ? set_errno_from_wsa() : rc;
}

ssize_t compat_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
  if (flags & MSG_DONTWAIT) {
    flags &= ~MSG_DONTWAIT;
    if (would_block(s, 1)) { errno = EAGAIN; return -1; }
  }
  int rc = sendto((SOCKET) s, (const char *) buf, (int) len, flags, to, tolen);
  return rc == SOCKET_ERROR ? set_errno_from_wsa() : rc;
}

int compat_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
  if (level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
      && optlen == (socklen_t) sizeof(struct timeval)) {
    /*
     * ROOT CAUSE FIX (Windows only): Winsock expects a DWORD in milliseconds
     * here. Handing it the POSIX struct timeval would be read as
     * "tv_sec milliseconds", e.g. {0, 100000} (100 ms) -> 0 -> infinite.
     */
    const struct timeval *tv = (const struct timeval *) optval;
    DWORD ms = (DWORD) (tv->tv_sec * 1000 + tv->tv_usec / 1000);
    if (ms == 0 && tv->tv_usec > 0) { ms = 1; }
    return setsockopt((SOCKET) s, level, optname, (const char *) &ms, sizeof(ms)) == SOCKET_ERROR
           ? set_errno_from_wsa() : 0;
  }
  return setsockopt((SOCKET) s, level, optname, (const char *) optval, optlen) == SOCKET_ERROR
         ? set_errno_from_wsa() : 0;
}

int compat_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen) {
  return getsockopt((SOCKET) s, level, optname, (char *) optval, optlen) == SOCKET_ERROR
         ? set_errno_from_wsa() : 0;
}

int compat_shutdown(int s, int how) {
  return shutdown((SOCKET) s, how) == SOCKET_ERROR ? set_errno_from_wsa() : 0;
}

int compat_getsockname(int s, struct sockaddr *name, socklen_t *namelen) {
  return getsockname((SOCKET) s, name, namelen) == SOCKET_ERROR ? set_errno_from_wsa() : 0;
}

int compat_getpeername(int s, struct sockaddr *name, socklen_t *namelen) {
  return getpeername((SOCKET) s, name, namelen) == SOCKET_ERROR ? set_errno_from_wsa() : 0;
}

static DWORD serial_rx_queue(serial_t *sp) {
  COMSTAT st;
  DWORD errs;
  if (!ClearCommError(sp->h, &errs, &st)) { return 0; }
  return st.cbInQue;
}

static int fdset_count(const fd_set *s) {
  return s ? (int) s->fd_count : 0;
}

/*
 * Winsock's select() only knows sockets. piHPSDR also select()s on
 * serial ports (CAT), and uses select() with empty sets as a sleep.
 * Both cases are emulated here, everything else goes to Winsock.
 */
int compat_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
  (void) nfds;
  int nserial = 0;
  if (r) {
    for (u_int i = 0; i < r->fd_count; i++) {
      if (serial_find((int) r->fd_array[i])) { nserial++; }
    }
  }
  if (fdset_count(r) + fdset_count(w) + fdset_count(e) == 0) {
    /* POSIX: select() without descriptors is a portable sleep */
    if (tv) { Sleep((DWORD) (tv->tv_sec * 1000 + tv->tv_usec / 1000)); }
    return 0;
  }
  if (nserial == 0) {
    int rc = select(0, r, w, e, tv);
    return rc == SOCKET_ERROR ? set_errno_from_wsa() : rc;
  }
  /* mixed/serial case: poll every 2 ms */
  ULONGLONG deadline = tv ? GetTickCount64() + (ULONGLONG) (tv->tv_sec * 1000 + tv->tv_usec / 1000) : 0;
  fd_set r_in = *r;
  fd_set w_in, e_in;
  if (w) { w_in = *w; }
  if (e) { e_in = *e; }
  for (;;) {
    int ready = 0;
    fd_set r_sock, r_out;
    FD_ZERO(&r_sock);
    FD_ZERO(&r_out);
    for (u_int i = 0; i < r_in.fd_count; i++) {
      serial_t *sp = serial_find((int) r_in.fd_array[i]);
      if (sp) {
        if (serial_rx_queue(sp) > 0) {
          FD_SET(r_in.fd_array[i], &r_out);
          ready++;
        }
      } else {
        FD_SET(r_in.fd_array[i], &r_sock);
      }
    }
    if (r_sock.fd_count > 0 || (w && w_in.fd_count > 0) || (e && e_in.fd_count > 0)) {
      struct timeval zero = {0, 0};
      if (w) { *w = w_in; }
      if (e) { *e = e_in; }
      int rc = select(0, r_sock.fd_count ? &r_sock : NULL, w, e, &zero);
      if (rc == SOCKET_ERROR) { return set_errno_from_wsa(); }
      for (u_int i = 0; i < r_sock.fd_count; i++) {
        FD_SET(r_sock.fd_array[i], &r_out);
      }
      ready += rc;
    }
    if (ready > 0 || (tv && GetTickCount64() >= deadline)) {
      *r = r_out;
      if (ready == 0) {
        if (w) { FD_ZERO(w); }
        if (e) { FD_ZERO(e); }
      }
      return ready;
    }
    Sleep(2);
  }
}

int inet_aton(const char *cp, struct in_addr *inp) {
  return inet_pton(AF_INET, cp, inp) == 1 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* getifaddrs() via GetAdaptersAddresses()                            */
/* ------------------------------------------------------------------ */

struct ifaddrs_block {
  struct ifaddrs     ifa;
  struct sockaddr_in addr;
  struct sockaddr_in mask;
  struct sockaddr_in bcast;
  char               name[IFNAMSIZ];
};

int getifaddrs(struct ifaddrs **ifap) {
  ULONG size = 16 * 1024;
  IP_ADAPTER_ADDRESSES *list = NULL;
  ULONG rc;
  *ifap = NULL;
  for (int tries = 0; tries < 3; tries++) {
    list = (IP_ADAPTER_ADDRESSES *) malloc(size);
    if (!list) { errno = ENOMEM; return -1; }
    rc = GetAdaptersAddresses(AF_INET,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              NULL, list, &size);
    if (rc != ERROR_BUFFER_OVERFLOW) { break; }
    free(list);
    list = NULL;
  }
  if (!list || rc != NO_ERROR) {
    free(list);
    errno = EIO;
    return -1;
  }
  struct ifaddrs *head = NULL, *tail = NULL;
  for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) { continue; }
      struct ifaddrs_block *b = (struct ifaddrs_block *) calloc(1, sizeof(*b));
      if (!b) { continue; }
      memcpy(&b->addr, u->Address.lpSockaddr, sizeof(b->addr));
      ULONG mask = 0;
      ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
      b->mask.sin_family = AF_INET;
      b->mask.sin_addr.s_addr = mask;
      b->bcast.sin_family = AF_INET;
      b->bcast.sin_addr.s_addr = b->addr.sin_addr.s_addr | ~mask;
      /* Friendly name ("Ethernet", "Wi-Fi") is what users recognise in logs/menus */
      if (a->FriendlyName == NULL
          || WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, b->name, sizeof(b->name), NULL, NULL) == 0) {
        snprintf(b->name, sizeof(b->name), "%s", a->AdapterName ? a->AdapterName : "if");
      }
      b->ifa.ifa_name = b->name;
      b->ifa.ifa_addr = (struct sockaddr *) &b->addr;
      b->ifa.ifa_netmask = (struct sockaddr *) &b->mask;
      b->ifa.ifa_broadaddr = (struct sockaddr *) &b->bcast;
      if (a->OperStatus == IfOperStatusUp) { b->ifa.ifa_flags |= IFF_UP | IFF_RUNNING; }
      if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        b->ifa.ifa_flags |= IFF_LOOPBACK;
      } else {
        b->ifa.ifa_flags |= IFF_BROADCAST | IFF_MULTICAST;
      }
      if (tail) { tail->ifa_next = &b->ifa; } else { head = &b->ifa; }
      tail = &b->ifa;
    }
  }
  free(list);
  *ifap = head;
  return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
  while (ifa) {
    struct ifaddrs *next = ifa->ifa_next;
    free(ifa);     /* ifa is the first member of its ifaddrs_block */
    ifa = next;
  }
}

/* ------------------------------------------------------------------ */
/* files and serial ports                                             */
/* ------------------------------------------------------------------ */

/*
 * "COM3" and "\\.\COM3" are serial ports. Linux names (/dev/ttyUSB0 etc.)
 * are deliberately NOT mapped: there is no reliable relation to COM numbers,
 * and opening a guessed COM port could talk to an unrelated device.
 */
static int serial_path(const char *path, char *out, size_t outlen) {
  if (!path) { return 0; }
  if (!strncmp(path, "\\\\.\\", 4)) {
    snprintf(out, outlen, "%s", path);
    return 1;
  }
  if (!_strnicmp(path, "COM", 3) && isdigit((unsigned char) path[3])) {
    snprintf(out, outlen, "\\\\.\\%s", path);
    return 1;
  }
  return 0;
}

static void serial_apply_lines(serial_t *sp) {
  EscapeCommFunction(sp->h, sp->dtr ? SETDTR : CLRDTR);
  EscapeCommFunction(sp->h, sp->rts ? SETRTS : CLRRTS);
}

int compat_open(const char *path, int flags, ...) {
  int mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  char dev[MAX_PATH];
  if (!serial_path(path, dev, sizeof(dev))) {
    /* regular file: POSIX has no text mode, so always open binary */
    int f = flags & ~O_NONBLOCK;
    if (!(f & _O_TEXT)) { f |= _O_BINARY; }
    if (path && !strncmp(path, "/tmp/", 5)) {
      /*
       * e.g. main.c's single-instance lock "/tmp/deskhpsdr.lock": there is
       * no /tmp on Windows (it would resolve to C:\tmp, which normally does
       * not exist, and piHPSDR would then refuse to start). Use %TEMP%.
       */
      char tmp[MAX_PATH];
      DWORD n = GetTempPathA(sizeof(tmp), tmp);
      if (n > 0 && n < sizeof(tmp)) {
        snprintf(dev, sizeof(dev), "%s%s", tmp, path + 5);
        return _open(dev, f, mode ? mode : (_S_IREAD | _S_IWRITE));
      }
    }
    return _open(path, f, mode ? mode : (_S_IREAD | _S_IWRITE));
  }
  HANDLE h = CreateFileA(dev, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT :
            (e == ERROR_ACCESS_DENIED) ? EBUSY : EIO;
    return -1;
  }
  /*
   * Reads never block inside the kernel (the VMIN/VTIME/O_NONBLOCK logic is
   * done in compat_read()): a blocking ReadFile() on a synchronous handle
   * would also stall WriteFile() from the other CAT threads.
   */
  COMMTIMEOUTS to;
  memset(&to, 0, sizeof(to));
  to.ReadIntervalTimeout = MAXDWORD;
  to.WriteTotalTimeoutConstant = 2000;
  SetCommTimeouts(h, &to);
  SetupComm(h, 4096, 4096);
  int fd = _open_osfhandle((intptr_t) h, _O_BINARY);
  if (fd < 0) {
    CloseHandle(h);
    errno = EMFILE;
    return -1;
  }
  EnterCriticalSection(&compat_lock);
  serial_t *sp = NULL;
  for (int i = 0; i < MAX_SERIAL_FDS; i++) {
    if (!serials[i].used) { sp = &serials[i]; break; }
  }
  if (sp) {
    memset(sp, 0, sizeof(*sp));
    sp->used = 1;
    sp->fd = fd;
    sp->h = h;
    sp->nonblock = (flags & O_NONBLOCK) != 0;
    sp->vmin = 1;
    sp->vtime = 0;
    sp->dtr = 1;      /* Linux raises DTR and RTS when a tty is opened */
    sp->rts = 1;
  }
  LeaveCriticalSection(&compat_lock);
  if (!sp) {
    _close(fd);
    errno = EMFILE;
    return -1;
  }
  serial_apply_lines(sp);
  return fd;
}

int compat_close(int fd) {
  serial_t *sp = serial_find(fd);
  if (sp) {
    EnterCriticalSection(&compat_lock);
    sp->used = 0;
    LeaveCriticalSection(&compat_lock);
    return _close(fd);     /* also closes the COM handle */
  }
  /*
   * Sockets are kernel handles, CRT fds are small integers: try the socket
   * first, fall back to the CRT for real files.
   */
  if (closesocket((SOCKET) fd) == 0) {
    sock_nonblock_set(fd, 0);
    return 0;
  }
  if (WSAGetLastError() == WSAENOTSOCK) { return _close(fd); }
  return set_errno_from_wsa();
}

static ssize_t serial_read(serial_t *sp, void *buf, size_t count) {
  ULONGLONG start = GetTickCount64();
  for (;;) {
    DWORD avail = serial_rx_queue(sp);
    if (avail > 0) {
      DWORD n = 0;
      if (!ReadFile(sp->h, buf, (DWORD) (count < avail ? count : avail), &n, NULL)) {
        errno = EIO;
        return -1;
      }
      return (ssize_t) n;
    }
    if (sp->nonblock) {
      errno = EAGAIN;
      return -1;
    }
    if (sp->vmin == 0) {
      /* VMIN=0: return 0 after VTIME tenths of a second */
      if (GetTickCount64() - start >= (ULONGLONG) sp->vtime * 100) { return 0; }
    }
    Sleep(2);
    if (!serial_find(sp->fd)) {    /* closed by another thread */
      errno = EBADF;
      return -1;
    }
  }
}

ssize_t compat_read(int fd, void *buf, size_t count) {
  serial_t *sp = serial_find(fd);
  if (sp) { return serial_read(sp, buf, count); }
  if (is_socket(fd)) { return compat_recv(fd, buf, count, 0); }
  return _read(fd, buf, (unsigned int) count);
}

ssize_t compat_write(int fd, const void *buf, size_t count) {
  serial_t *sp = serial_find(fd);
  if (sp) {
    DWORD n = 0;
    if (!WriteFile(sp->h, buf, (DWORD) count, &n, NULL)) {
      errno = EIO;
      return -1;
    }
    return (ssize_t) n;
  }
  if (is_socket(fd)) { return compat_send(fd, buf, count, 0); }
  return _write(fd, buf, (unsigned int) count);
}

int compat_fcntl(int fd, int cmd, ...) {
  long arg = 0;
  va_list ap;
  va_start(ap, cmd);
  if (cmd == F_SETFL || cmd == F_SETFD) { arg = va_arg(ap, long); }
  va_end(ap);
  serial_t *sp = serial_find(fd);
  switch (cmd) {
  case F_GETFD:
  case F_SETFD:
    return 0;
  case F_GETFL:
    if (sp) { return O_RDWR | (sp->nonblock ? O_NONBLOCK : 0); }
    if (is_socket(fd)) { return O_RDWR | (sock_nonblock_get(fd) ? O_NONBLOCK : 0); }
    return 0;
  case F_SETFL:
    if (sp) {
      sp->nonblock = (arg & O_NONBLOCK) != 0;
      return 0;
    }
    if (is_socket(fd)) {
      u_long nb = (arg & O_NONBLOCK) ? 1 : 0;
      if (ioctlsocket((SOCKET) fd, FIONBIO, &nb) == SOCKET_ERROR) { return set_errno_from_wsa(); }
      sock_nonblock_set(fd, (int) nb);
      return 0;
    }
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

int compat_ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  serial_t *sp = serial_find(fd);
  if (sp) {
    int *bits = (int *) arg;
    DWORD ms = 0;
    switch (request) {
    case TIOCMGET:
      GetCommModemStatus(sp->h, &ms);
      *bits = (sp->dtr ? TIOCM_DTR : 0) | (sp->rts ? TIOCM_RTS : 0)
              | ((ms & MS_CTS_ON) ? TIOCM_CTS : 0) | ((ms & MS_DSR_ON) ? TIOCM_DSR : 0)
              | ((ms & MS_RING_ON) ? TIOCM_RNG : 0) | ((ms & MS_RLSD_ON) ? TIOCM_CAR : 0);
      return 0;
    case TIOCMSET:
      sp->dtr = (*bits & TIOCM_DTR) != 0;
      sp->rts = (*bits & TIOCM_RTS) != 0;
      serial_apply_lines(sp);
      return 0;
    case TIOCMBIS:
      if (*bits & TIOCM_DTR) { sp->dtr = 1; }
      if (*bits & TIOCM_RTS) { sp->rts = 1; }
      serial_apply_lines(sp);
      return 0;
    case TIOCMBIC:
      if (*bits & TIOCM_DTR) { sp->dtr = 0; }
      if (*bits & TIOCM_RTS) { sp->rts = 0; }
      serial_apply_lines(sp);
      return 0;
    case FIONREAD:
      *bits = (int) serial_rx_queue(sp);
      return 0;
    default:
      errno = ENOTTY;
      return -1;
    }
  }
  if (request == FIONREAD || request == FIONBIO) {
    u_long v = (request == FIONBIO) ? (u_long) * (int *) arg : 0;
    if (ioctlsocket((SOCKET) fd, (long) request, &v) == SOCKET_ERROR) { return set_errno_from_wsa(); }
    if (request == FIONREAD) { *(int *) arg = (int) v; }
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

/* ------------------------------------------------------------------ */
/* termios on top of DCB                                              */
/* ------------------------------------------------------------------ */

int tcgetattr(int fd, struct termios *t) {
  serial_t *sp = serial_find(fd);
  DCB dcb;
  if (!sp) { errno = ENOTTY; return -1; }
  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(sp->h, &dcb)) { errno = EIO; return -1; }
  memset(t, 0, sizeof(*t));
  t->c_ispeed = t->c_ospeed = dcb.BaudRate;
  switch (dcb.ByteSize) {
  case 5: t->c_cflag |= CS5; break;
  case 6: t->c_cflag |= CS6; break;
  case 7: t->c_cflag |= CS7; break;
  default: t->c_cflag |= CS8; break;
  }
  if (dcb.Parity != NOPARITY) { t->c_cflag |= PARENB; }
  if (dcb.Parity == ODDPARITY) { t->c_cflag |= PARODD; }
  if (dcb.StopBits == TWOSTOPBITS) { t->c_cflag |= CSTOPB; }
  if (dcb.fOutxCtsFlow) { t->c_cflag |= CRTSCTS; }
  if (dcb.fOutX) { t->c_iflag |= IXON; }
  if (dcb.fInX) { t->c_iflag |= IXOFF; }
  t->c_cflag |= CREAD | CLOCAL;
  t->c_cc[VMIN] = (cc_t) sp->vmin;
  t->c_cc[VTIME] = (cc_t) sp->vtime;
  return 0;
}

int tcsetattr(int fd, int action, const struct termios *t) {
  (void) action;
  serial_t *sp = serial_find(fd);
  DCB dcb;
  if (!sp) { errno = ENOTTY; return -1; }
  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(sp->h, &dcb)) { errno = EIO; return -1; }
  if (t->c_ospeed) { dcb.BaudRate = t->c_ospeed; }
  switch (t->c_cflag & CSIZE) {
  case CS5: dcb.ByteSize = 5; break;
  case CS6: dcb.ByteSize = 6; break;
  case CS7: dcb.ByteSize = 7; break;
  default:  dcb.ByteSize = 8; break;
  }
  dcb.fBinary = TRUE;
  dcb.fParity = (t->c_cflag & PARENB) ? TRUE : FALSE;
  dcb.Parity = (t->c_cflag & PARENB) ? ((t->c_cflag & PARODD) ? ODDPARITY : EVENPARITY) : NOPARITY;
  dcb.StopBits = (t->c_cflag & CSTOPB) ? TWOSTOPBITS : ONESTOPBIT;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDsrSensitivity = FALSE;
  /* termios never touches DTR/RTS (unless hardware handshake): keep tracked state */
  dcb.fDtrControl = sp->dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
  if (t->c_cflag & CRTSCTS) {
    dcb.fOutxCtsFlow = TRUE;
    dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
  } else {
    dcb.fOutxCtsFlow = FALSE;
    dcb.fRtsControl = sp->rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
  }
  dcb.fOutX = (t->c_iflag & IXON) ? TRUE : FALSE;
  dcb.fInX = (t->c_iflag & IXOFF) ? TRUE : FALSE;
  dcb.XonChar = 0x11;
  dcb.XoffChar = 0x13;
  dcb.XonLim = 256;
  dcb.XoffLim = 256;
  dcb.fNull = FALSE;
  dcb.fAbortOnError = FALSE;
  if (!SetCommState(sp->h, &dcb)) { errno = EINVAL; return -1; }
  sp->vmin = t->c_cc[VMIN];
  sp->vtime = t->c_cc[VTIME];
  return 0;
}

int tcflush(int fd, int queue) {
  serial_t *sp = serial_find(fd);
  if (!sp) { errno = ENOTTY; return -1; }
  DWORD f = 0;
  if (queue == TCIFLUSH || queue == TCIOFLUSH) { f |= PURGE_RXCLEAR | PURGE_RXABORT; }
  if (queue == TCOFLUSH || queue == TCIOFLUSH) { f |= PURGE_TXCLEAR | PURGE_TXABORT; }
  PurgeComm(sp->h, f);
  return 0;
}

int tcdrain(int fd) {
  serial_t *sp = serial_find(fd);
  if (!sp) { errno = ENOTTY; return -1; }
  FlushFileBuffers(sp->h);
  return 0;
}

int cfsetispeed(struct termios *t, speed_t s) { t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { t->c_ospeed = s; return 0; }
speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }

void cfmakeraw(struct termios *t) {
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(CSIZE | PARENB);
  t->c_cflag |= CS8;
}

/* ------------------------------------------------------------------ */
/* misc                                                               */
/* ------------------------------------------------------------------ */

int compat_rename(const char *oldpath, const char *newpath) {
  /* POSIX semantics: atomically replace an existing target */
  if (MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { return 0; }
  DWORD e = GetLastError();
  errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT :
          (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) ? EACCES : EIO;
  return -1;
}

int posix_memalign(void **p, size_t align, size_t size) {
  /* Saturn/XDMA only, never active on Windows (see win_compat_fd.h) */
  (void) align;
  (void) size;
  *p = NULL;
  return ENOMEM;
}

int compat_mkdir(const char *path, int mode) {
  (void) mode;
  return _mkdir(path);
}

int compat_kill(pid_t pid, int sig) {
  /* only used for the (non-existing on Windows) rigctld helper */
  (void) pid;
  (void) sig;
  errno = ESRCH;
  return -1;
}

pid_t waitpid(pid_t pid, int *status, int options) {
  (void) pid;
  (void) options;
  if (status) { *status = 0; }
  errno = ECHILD;
  return -1;
}

unsigned int compat_getuid(void) {
  return 1000;   /* "not root" */
}

struct passwd *getpwuid(unsigned int uid) {
  static struct passwd pw;
  static char dir[MAX_PATH];
  (void) uid;
  const char *p = getenv("USERPROFILE");
  if (!p) { return NULL; }
  snprintf(dir, sizeof(dir), "%s", p);
  pw.pw_name = getenv("USERNAME");
  pw.pw_dir = dir;
  pw.pw_shell = NULL;
  return &pw;
}

int uname(struct utsname *u) {
  typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
  OSVERSIONINFOW v;
  DWORD n = sizeof(u->nodename);
  memset(u, 0, sizeof(*u));
  memset(&v, 0, sizeof(v));
  v.dwOSVersionInfoSize = sizeof(v);
  /* GetVersionEx() lies for non-manifested programs; RtlGetVersion() does not */
  RtlGetVersion_t rtl = (RtlGetVersion_t) (void *) GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
  if (rtl) { rtl(&v); }
  snprintf(u->sysname, sizeof(u->sysname), "Windows");
  snprintf(u->release, sizeof(u->release), "%lu.%lu", (unsigned long) v.dwMajorVersion, (unsigned long) v.dwMinorVersion);
  snprintf(u->version, sizeof(u->version), "Build %lu", (unsigned long) v.dwBuildNumber);
#if defined(__x86_64__)
  snprintf(u->machine, sizeof(u->machine), "x86_64");
#elif defined(__aarch64__)
  snprintf(u->machine, sizeof(u->machine), "aarch64");
#else
  snprintf(u->machine, sizeof(u->machine), "unknown");
#endif
  if (!GetComputerNameA(u->nodename, &n)) { snprintf(u->nodename, sizeof(u->nodename), "localhost"); }
  return 0;
}

static void ft_to_tv(const FILETIME *ft, struct timeval *tv) {
  ULONGLONG t = ((ULONGLONG) ft->dwHighDateTime << 32) | ft->dwLowDateTime;   /* 100 ns units */
  tv->tv_sec = (long) (t / 10000000ULL);
  tv->tv_usec = (long) ((t % 10000000ULL) / 10);
}

int getrusage(int who, struct rusage *usage) {
  FILETIME c, e, k, u;
  memset(usage, 0, sizeof(*usage));
  BOOL ok = (who == RUSAGE_THREAD) ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)
            : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  if (!ok) { errno = EINVAL; return -1; }
  ft_to_tv(&u, &usage->ru_utime);
  ft_to_tv(&k, &usage->ru_stime);
  return 0;
}

int getpriority(int which, int who) {
  (void) which;
  (void) who;
  DWORD c = GetPriorityClass(GetCurrentProcess());
  errno = 0;
  if (c == HIGH_PRIORITY_CLASS || c == REALTIME_PRIORITY_CLASS) { return -15; }
  if (c == ABOVE_NORMAL_PRIORITY_CLASS) { return -10; }
  if (c == BELOW_NORMAL_PRIORITY_CLASS) { return 10; }
  if (c == IDLE_PRIORITY_CLASS) { return 19; }
  return 0;
}

int setpriority(int which, int who, int prio) {
  (void) which;
  (void) who;
  /* nice -10 (what main.c asks for) ~ "above normal"; never REALTIME */
  DWORD c = prio < 0 ? ABOVE_NORMAL_PRIORITY_CLASS : (prio > 0 ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
  if (!SetPriorityClass(GetCurrentProcess(), c)) { errno = EACCES; return -1; }
  return 0;
}

int flock(int fd, int operation) {
  HANDLE h = (HANDLE) _get_osfhandle(fd);
  OVERLAPPED ov;
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  memset(&ov, 0, sizeof(ov));
  if (operation & LOCK_UN) {
    return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
  }
  DWORD f = 0;
  if (operation & LOCK_EX) { f |= LOCKFILE_EXCLUSIVE_LOCK; }
  if (operation & LOCK_NB) { f |= LOCKFILE_FAIL_IMMEDIATELY; }
  if (!LockFileEx(h, f, 0, MAXDWORD, MAXDWORD, &ov)) {
    errno = EWOULDBLOCK;
    return -1;
  }
  return 0;
}

#endif /* _WIN32 */
