/* Copyright (C)
* 2026 - piHPSDR Windows (MSYS2 MinGW64) port
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*/

/*
 * Light-weight part of the Windows POSIX compatibility layer (see
 * win_compat.h for the full rationale): file-descriptor level wrappers.
 *
 * WHY separate: this part is pulled in through the <unistd.h>/<fcntl.h>/
 * <sys/stat.h> shims, i.e. by almost every source file. It deliberately does
 * NOT include <windows.h>, so that the Win32 macro zoo (ERROR, TRANSPARENT,
 * interface, ...) cannot clash with piHPSDR identifiers in files that do
 * no networking at all.
 */

#ifndef PIHPSDR_WIN_COMPAT_FD_H
#define PIHPSDR_WIN_COMPAT_FD_H

#ifdef _WIN32

#include <sys/types.h>
#include <stddef.h>
/*
 * Pull in every CRT header that declares open/close/read/write/mkdir
 * *before* the redirecting macros below are defined. Otherwise a later
 * #include of one of them would have its prototypes rewritten by the macros.
 */
#include <io.h>
#include <direct.h>
#include <process.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* file descriptor wrappers (sockets / serial ports / files)          */
/* ------------------------------------------------------------------ */

#ifndef O_NONBLOCK
  #define O_NONBLOCK 0x00100000   /* bit unused by the MS CRT */
#endif
#ifndef O_NOCTTY
  #define O_NOCTTY 0
#endif
#ifndef O_SYNC
  #define O_SYNC 0
#endif
#ifndef O_NDELAY
  #define O_NDELAY O_NONBLOCK
#endif
#ifndef O_CLOEXEC
  #define O_CLOEXEC 0
#endif
#ifndef F_GETFL
  #define F_GETFD 1
  #define F_SETFD 2
  #define F_GETFL 3
  #define F_SETFL 4
#endif
#ifndef FD_CLOEXEC
  #define FD_CLOEXEC 1
#endif

int     compat_open(const char *path, int flags, ...);
int     compat_close(int fd);
ssize_t compat_read(int fd, void *buf, size_t count);
ssize_t compat_write(int fd, const void *buf, size_t count);
int     compat_fcntl(int fd, int cmd, ...);
int     compat_ioctl(int fd, unsigned long request, ...);

/* The CRT names are left intact; piHPSDR's calls are redirected. */
#define open(...)          compat_open(__VA_ARGS__)
#define close(fd)          compat_close((int)(fd))
#define read(fd,b,n)       compat_read((fd),(b),(n))
#define write(fd,b,n)      compat_write((fd),(b),(n))
#define fcntl(...)         compat_fcntl(__VA_ARGS__)
#define ioctl(fd,r,...)    compat_ioctl((int)(fd),(unsigned long)(r),__VA_ARGS__)

/* mingw's mkdir() has no mode argument */
int compat_mkdir(const char *path, int mode);
#define mkdir(p,m)         compat_mkdir((p),(m))

/* realpath()/fsync()/sync() are missing in the MS CRT */
#include <stdlib.h>
#include <errno.h>
#ifndef PATH_MAX
  #define PATH_MAX 260
#endif
static inline char *realpath(const char *path, char *resolved) {
  /* POSIX realpath() fails for non-existing paths (radio.c uses that to
     probe for /dev/radioberry); _fullpath() alone would not. */
  if (_access(path, 0) != 0) { return NULL; }
  return _fullpath(resolved, path, PATH_MAX);   /* allocates if resolved==NULL, like POSIX */
}
static inline int fsync(int fd) { return _commit(fd); }
static inline void sync(void) { }

/* no symbolic links in the MS CRT stat() (startup.c) */
#ifndef S_ISLNK
  #define S_ISLNK(m) 0
#endif

/*
 * pread()/pwrite()/posix_memalign() are only used by the Saturn/XDMA
 * driver code (saturndrivers.c, saturnmain.c), which is always compiled
 * but only becomes active if the /dev/xdma0_* devices of a Saturn (G2)
 * radio's Raspberry Pi exist - never on Windows. Minimal, non-atomic
 * versions are enough to link; posix_memalign() deliberately fails
 * (memory from _aligned_malloc() must not be released with free()).
 */
static inline ssize_t pread(int fd, void *buf, size_t n, off_t off) {
  if (_lseeki64(fd, off, 0 /* SEEK_SET */) < 0) { return -1; }
  return _read(fd, buf, (unsigned int) n);
}
static inline ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
  if (_lseeki64(fd, off, 0 /* SEEK_SET */) < 0) { return -1; }
  return _write(fd, buf, (unsigned int) n);
}
int posix_memalign(void **p, size_t align, size_t size);   /* win_compat.c */

/* ------------------------------------------------------------------ */
/* misc. POSIX process stuff                                          */
/* ------------------------------------------------------------------ */

#ifndef SIGPIPE
  #define SIGPIPE 13    /* signal(SIGPIPE, ...) simply fails (harmless) */
#endif
#ifndef SIGKILL
  #define SIGKILL 9
#endif
#ifndef SIGHUP
  #define SIGHUP 1
#endif
#ifndef SIGUSR1
  #define SIGUSR1 30
#endif

int compat_kill(pid_t pid, int sig);
#define kill(p,s) compat_kill((p),(s))
pid_t waitpid(pid_t pid, int *status, int options);
#ifndef WNOHANG
  #define WNOHANG 1
#endif

unsigned int compat_getuid(void);
#define getuid()  compat_getuid()
#define geteuid() compat_getuid()

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* PIHPSDR_WIN_COMPAT_FD_H */
