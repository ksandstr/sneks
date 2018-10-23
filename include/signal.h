
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stdint.h>

/* from the host environment, because it appears to work. */
#include <bits/signum.h>


typedef uint64_t sigset_t;
typedef volatile int sig_atomic_t;
typedef void (*__sighandler_t)(int);
typedef __sighandler_t sighandler_t;


/* straight outta the Linux sigaction(2) man page, with the "since Linux
 * x.y.z" ones dropped and untested ones if'd out.
 */
typedef struct __siginfo_s
{
	int si_signo, si_errno, si_code, si_trapno, si_status;
	int si_pid, si_uid;
#if 0
	pid_t si_pid;
	uid_t si_uid;
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


struct sigaction
{
	__sighandler_t sa_handler;
	void (*sa_sigaction)(int, siginfo_t *, void *);
	sigset_t sa_mask;
	int sa_flags;
};


#define SA_NOCLDSTOP 1			/* don't send SIGCHLD when children stop. */
#define SA_NOCLDWAIT 2			/* don't create zombie on child death. */
#define SA_SIGINFO 4			/* call sa_sigaction instead of sa_handler */
#define SA_RESTART 0x10000000	/* restart syscall on signal return */


extern int sigaction(int signum,
	const struct sigaction *act, struct sigaction *oldact);

extern sighandler_t signal(int signum, sighandler_t handler);

extern int kill(int __pid, int __sig);


#endif
