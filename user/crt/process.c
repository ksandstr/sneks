
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>

#include "proc-defs.h"
#include "private.h"


int getpid(void) {
	/* well isn't that just fucking twee */
	return pidof_NP(L4_MyGlobalId());
}


void exit(int status)
{
	int n = __proc_exit(__the_sysinfo->api.proc, status);
	fprintf(stderr, "Proc::exit returned n=%d\n", n);
	/* the alternative. */
	for(;;) {
		asm volatile ("int $69");	/* the sex number */
		L4_Set_ExceptionHandler(L4_nilthread);
		asm volatile ("int $96");	/* the weird sex number */
		L4_Sleep(L4_Never);
	}
}


int atexit(void (*fn)(void))
{
	/* failure: implementation missing (oopsie) */
	return -1;
}


pid_t wait(int *status_p) {
	return waitpid(-1, status_p, 0);
}


pid_t waitpid(pid_t pid, int *wstatus, int options)
{
	siginfo_t si;
	int n = waitid(pid == -1 ? P_ANY : P_PID, pid, &si, options);
	if(n != 0) return n;
	if(wstatus != NULL) {
		/* FIXME: get macros. for now it's just low bit for exit/not, rest is
		 * status or signal number. add asserts after each to confirm that
		 * translation is correct.
		 */
		switch(si.si_code) {
			case CLD_EXITED: *wstatus = si.si_status << 1 | 1; break;
			case CLD_KILLED: *wstatus = si.si_status << 2; break;
			case CLD_DUMPED: *wstatus = si.si_status << 2 | 2; break;
			case CLD_STOPPED:
			case CLD_TRAPPED:
			case CLD_CONTINUED:
				fprintf(stderr, "%s: no support for stopped/trapped/continued\n",
					__func__);
				abort();
			default: return -1;	/* FIXME: remove this stuff */
		}
	}

	return si.si_pid;
}


int waitid(idtype_t idtype, id_t id, siginfo_t *si, int options)
{
	siginfo_t dummy;
	if(si == NULL) si = &dummy;
	int n = __proc_wait(__the_sysinfo->api.proc,
		&si->si_pid, &si->si_uid, &si->si_signo,
		&si->si_status, &si->si_code,
		idtype, id, options);
	if(n != 0) {
		/* FIXME: translate errno */
		return -1;
	}
	return 0;
}
