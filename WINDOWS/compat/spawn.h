/* Windows (MinGW64) shim for <spawn.h>.
 * posix_spawn() is only used to start the external helper
 * "rigctld_deskhpsdr", which does not exist on Windows. The stub fails
 * with ENOSYS, so rigctl.c just disables that option as it does on any
 * system where the helper is missing. */
#pragma once
#include <errno.h>
#include <sys/types.h>
typedef int posix_spawn_file_actions_t;
typedef int posix_spawnattr_t;
static inline int posix_spawn(pid_t *pid, const char *path,
                              const posix_spawn_file_actions_t *fa,
                              const posix_spawnattr_t *attr,
                              char *const argv[], char *const envp[]) {
  (void) pid; (void) path; (void) fa; (void) attr; (void) argv; (void) envp;
  return ENOSYS;
}
#include <stdlib.h>   /* environ */
