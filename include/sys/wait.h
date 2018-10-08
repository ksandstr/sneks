
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

/* TODO: don't include sys/types.h, use bits/sys/types.h and the
 * double-underscore versions of foo_t instead.
 */
#include <sys/types.h>
#include <sneks/process.h>


#define WIFEXITED(st) (((st) & 1) != 0)
#define WEXITSTATUS(st) ((st) >> 1)
#define WIFSIGNALED(st) (((st) & 3) == 0)
#define WTERMSIG(st) ((st) >> 2)
#define WCOREDUMP(st) (((st) & 3) == 2)
/* FIXME: fill these in once they can be tested */
#define WIFSTOPPED(st) 0
#define WSTOPSIG(st) 0
#define WIFCONTINUED(st) 0


extern pid_t wait(int *wstatus);
extern pid_t waitpid(pid_t pid, int *wstatus, int options);
extern int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

#endif
