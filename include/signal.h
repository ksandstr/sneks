#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stdint.h>
#include <sys/types.h>
#include <bits/signal.h>

typedef struct __sigset_t {
	unsigned long __bits[128 / sizeof(long)];
} sigset_t;

typedef int sig_atomic_t;
typedef void (*__sighandler_t)(int);
typedef __sighandler_t sighandler_t;

typedef struct __stack_s {
	void *ss_sp;
	size_t ss_size;
	int ss_flags;
} stack_t;

/* straight outta the Linux sigaction(2) man page, with the "since Linux
 * x.y.z" ones dropped and untested ones if'd out.
 */
typedef struct __siginfo_s
{
	int si_signo, si_errno, si_code, si_trapno, si_status;
	pid_t si_pid;
	uid_t si_uid;
#if 0
	clock_t si_utime, si_stime;
	sigval_t si_value;
	int si_int;
	void *si_ptr;
	int si_overrun, si_timerid;
	void *si_addr;
	long si_band;
	int si_fd;
#endif
} siginfo_t;

struct sigaction {
	__sighandler_t sa_handler;
	void (*sa_sigaction)(int, siginfo_t *, void *);
	sigset_t sa_mask;
	int sa_flags;
};

#define SA_NOCLDSTOP 1			/* don't send SIGCHLD when children stop. */
#define SA_NOCLDWAIT 2			/* don't create zombie on child death. */
#define SA_SIGINFO 4			/* call sa_sigaction instead of sa_handler */
#define SA_RESTART 0x10000000	/* restart syscall on signal return */
#define SA_NODEFER 0x40000000	/* don't mask signal within handler */

/* `how' of sigprocmask(2). */
#define SIG_BLOCK 0				/* add to block set */
#define SIG_UNBLOCK 1			/* remove from block set & trigger pending */
#define SIG_SETMASK 2			/* set mask & trigger pending if unblocked */

/* SIGCHLD codes */
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

/* realtime signals. */
#define SIGRTMIN (SIGSYS + 1)
#define SIGRTMAX (_NSIG - 1)

/* other constants */
#define SIG_ERR ((__sighandler_t)-1)
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)

extern int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
extern sighandler_t signal(int signum, sighandler_t handler);

extern int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
extern int sigpending(sigset_t *set);
extern int sigsuspend(const sigset_t *mask);

extern int kill(pid_t __pid, int __sig);
extern int raise(int __sig);

/* sigsetops(3) */
extern int sigemptyset(sigset_t *set);
extern int sigfillset(sigset_t *set);
extern int sigaddset(sigset_t *set, int signum);
extern int sigdelset(sigset_t *set, int signum);
extern int sigismember(sigset_t *set, int signum);

#endif
