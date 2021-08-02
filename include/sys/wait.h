
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

/* TODO: only include id_t and pid_t from sys/types.h */
#include <sys/types.h>
#include <sneks/process.h>


#define WIFEXITED(st) (!!((st) & 1))
#define WEXITSTATUS(st) (((st) >> 1) & 0xff)
#define WIFSIGNALED(st) (((st) & 3) == 0)
#define WTERMSIG(st) (((st) >> 2) & 0xff)
#define WCOREDUMP(st) (((st) & 3) == 2)
/* FIXME: fill these in once they can be tested */
#define WIFSTOPPED(st) 0
#define WSTOPSIG(st) 0
#define WIFCONTINUED(st) 0


/* constants for waitid(2). */
typedef enum __idtype {
	P_ALL = 0,
	P_PID = 1,
	P_PGID = 2,
} idtype_t;

#define WNOHANG 1	/* return 0 instead of blocking */
#define WUNTRACED 2	/* report status of stopped children */
/* TODO: WSTOPPED, WEXITED, WCONTINUED, WNOWAIT */


struct __siginfo_s;


extern pid_t wait(int *wstatus);
extern pid_t waitpid(pid_t pid, int *wstatus, int options);
extern int waitid(idtype_t idtype, id_t id, struct __siginfo_s *infop, int options);

#endif
