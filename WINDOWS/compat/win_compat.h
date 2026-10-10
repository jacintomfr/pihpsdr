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
 * POSIX compatibility layer for building piHPSDR natively on Windows.
 *
 * WHY this exists: piHPSDR is written against POSIX (BSD sockets,
 * getifaddrs, termios, fcntl, ...). Instead of sprinkling hundreds of
 * #ifdef _WIN32 through code that is shared with the Linux/macOS upstream
 * (which would make every upstream merge a conflict fest), the Windows
 * build puts WINDOWS/compat first on the include path. It provides the
 * missing POSIX headers (sys/socket.h, termios.h, ifaddrs.h, ...) which
 * all funnel into this file. Here, the few POSIX calls whose *semantics*
 * differ on Windows are redirected to wrappers in win_compat.c.
 *
 * Semantic differences handled here (the reasons behind the wrappers):
 *  - Sockets are not file descriptors on Windows: close()/read()/write()
 *    on a socket must become closesocket()/recv()/send().
 *  - Winsock reports errors via WSAGetLastError(), not errno. The code
 *    checks errno (EINPROGRESS, EAGAIN, ...) so the wrappers map it.
 *  - SO_RCVTIMEO/SO_SNDTIMEO take a DWORD in milliseconds on Windows, not
 *    a struct timeval. Passing a timeval silently gives a wrong timeout
 *    (e.g. {0, 100000} would become "wait forever").
 *  - fcntl(O_NONBLOCK) does not exist -> ioctlsocket(FIONBIO).
 *  - A UDP socket on Windows reports an ICMP "port unreachable" as a
 *    WSAECONNRESET error on the *next* recvfrom(), which would terminate
 *    the HPSDR receive threads. SIO_UDP_CONNRESET is switched off.
 *  - Serial ports (CAT, serial PTT/TUNE) are opened as COMx with the
 *    Win32 comm API; termios/TIOCM ioctls are emulated on top of it.
 *
 * This header is only ever seen by the Windows build.
 */

#ifndef PIHPSDR_WIN_COMPAT_H
#define PIHPSDR_WIN_COMPAT_H

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
/* NOTE: <iphlpapi.h> is deliberately only included by win_compat.c: it drags
   in OLE (objidl.h), whose 'SNB' typedef clashes with piHPSDR's action SNB. */
#include <io.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>
#include "win_compat_fd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* missing POSIX socket constants                                     */
/* ------------------------------------------------------------------ */

#ifndef SO_REUSEPORT
  /* Windows has no SO_REUSEPORT; SO_REUSEADDR has the "share the port"
     semantics piHPSDR asks for with REUSEPORT. */
  #define SO_REUSEPORT SO_REUSEADDR
#endif
#ifndef SOL_TCP
  #define SOL_TCP IPPROTO_TCP
#endif
#ifndef SOL_IP
  #define SOL_IP IPPROTO_IP
#endif
#ifndef EHOSTDOWN
  #define EHOSTDOWN 1064      /* not in the MS CRT errno set */
#endif
#ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0      /* there is no SIGPIPE on Windows */
#endif
#ifndef MSG_DONTWAIT
  #define MSG_DONTWAIT 0x40000000   /* emulated in compat_recv/compat_send */
#endif
#ifndef SHUT_RD
  #define SHUT_RD   SD_RECEIVE
  #define SHUT_WR   SD_SEND
  #define SHUT_RDWR SD_BOTH
#endif
#ifndef IFNAMSIZ
  #define IFNAMSIZ 64         /* Windows "friendly" adapter names are long */
#endif
#ifndef IF_NAMESIZE
  #define IF_NAMESIZE IFNAMSIZ
#endif
#ifndef IFF_UP
  #define IFF_UP 0x1
#endif
#ifndef IFF_BROADCAST
  #define IFF_BROADCAST 0x2
#endif
#ifndef IFF_LOOPBACK
  #define IFF_LOOPBACK 0x4
#endif
#ifndef IFF_POINTOPOINT
  #define IFF_POINTOPOINT 0x10
#endif
#ifndef IFF_RUNNING
  #define IFF_RUNNING 0x40
#endif
#ifndef IFF_MULTICAST
  #define IFF_MULTICAST 0x1000
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

/* ------------------------------------------------------------------ */
/* <endian.h> byte order helpers (glibc gets them via netinet/in.h;   */
/* used by client_server.h). Windows on x86-64 is little endian.      */
/* ------------------------------------------------------------------ */

#ifndef htobe64
  #define htobe16(x) __builtin_bswap16(x)
  #define htole16(x) (x)
  #define be16toh(x) __builtin_bswap16(x)
  #define le16toh(x) (x)
  #define htobe32(x) __builtin_bswap32(x)
  #define htole32(x) (x)
  #define be32toh(x) __builtin_bswap32(x)
  #define le32toh(x) (x)
  #define htobe64(x) __builtin_bswap64(x)
  #define htole64(x) (x)
  #define be64toh(x) __builtin_bswap64(x)
  #define le64toh(x) (x)
#endif

/* ------------------------------------------------------------------ */
/* getifaddrs() emulation (GetAdaptersAddresses)                      */
/* ------------------------------------------------------------------ */

struct ifaddrs {
  struct ifaddrs  *ifa_next;
  char            *ifa_name;
  unsigned int     ifa_flags;
  struct sockaddr *ifa_addr;
  struct sockaddr *ifa_netmask;
  union {
    struct sockaddr *ifu_broadaddr;
    struct sockaddr *ifu_dstaddr;
  } ifa_ifu;
  void            *ifa_data;
};
#ifndef ifa_broadaddr
  #define ifa_broadaddr ifa_ifu.ifu_broadaddr
  #define ifa_dstaddr   ifa_ifu.ifu_dstaddr
#endif

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

int inet_aton(const char *cp, struct in_addr *inp);

/* ------------------------------------------------------------------ */
/* socket call wrappers (errno mapping, timeval conversion, ...)      */
/* ------------------------------------------------------------------ */

int     compat_socket(int domain, int type, int protocol);
int     compat_connect(int s, const struct sockaddr *name, int namelen);
int     compat_bind(int s, const struct sockaddr *name, int namelen);
int     compat_listen(int s, int backlog);
int     compat_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
ssize_t compat_recv(int s, void *buf, size_t len, int flags);
ssize_t compat_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
ssize_t compat_send(int s, const void *buf, size_t len, int flags);
ssize_t compat_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
int     compat_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int     compat_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);
int     compat_shutdown(int s, int how);
int     compat_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
int     compat_getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int     compat_getpeername(int s, struct sockaddr *name, socklen_t *namelen);

#define socket(d,t,p)                 compat_socket((d),(t),(p))
#define connect(s,n,l)                compat_connect((int)(s),(n),(int)(l))
#define bind(s,n,l)                   compat_bind((int)(s),(n),(int)(l))
#define listen(s,b)                   compat_listen((int)(s),(b))
#define accept(s,a,l)                 compat_accept((int)(s),(a),(socklen_t *)(l))
#define recv(s,b,l,f)                 compat_recv((int)(s),(b),(l),(f))
#define recvfrom(s,b,l,f,a,al)        compat_recvfrom((int)(s),(b),(l),(f),(a),(socklen_t *)(al))
#define send(s,b,l,f)                 compat_send((int)(s),(b),(l),(f))
#define sendto(s,b,l,f,a,al)          compat_sendto((int)(s),(b),(l),(f),(a),(al))
#define setsockopt(s,lv,o,v,l)        compat_setsockopt((int)(s),(lv),(o),(v),(socklen_t)(l))
#define getsockopt(s,lv,o,v,l)        compat_getsockopt((int)(s),(lv),(o),(v),(socklen_t *)(l))
#define shutdown(s,h)                 compat_shutdown((int)(s),(h))
#define select(n,r,w,e,t)             compat_select((n),(r),(w),(e),(t))
#define getsockname(s,n,l)            compat_getsockname((int)(s),(n),(socklen_t *)(l))
#define getpeername(s,n,l)            compat_getpeername((int)(s),(n),(socklen_t *)(l))

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* PIHPSDR_WIN_COMPAT_H */
