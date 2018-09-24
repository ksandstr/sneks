
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

/* TODO: don't include sys/types.h, use bits/sys/types.h and the
 * double-underscore versions of foo_t instead.
 */
#include <sys/types.h>
#include <sneks/process.h>


extern pid_t wait(int *wstatus);
extern pid_t waitpid(pid_t pid, int *wstatus, int options);
extern int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

#endif
