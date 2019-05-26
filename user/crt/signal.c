
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "proc-defs.h"
#include "private.h"


int pause(void)
{
	bool warned = false;
	L4_MsgTag_t tag;
	do {
		/* sleep in a self-send phase that never ends to enable fast siginvoke. */
		L4_LoadMR(0, 0);
		tag = L4_Send(L4_Myself());
		if((L4_IpcSucceeded(tag) || L4_ErrorCode() != 6) && !warned) {
			fprintf(stderr, "%s: odd ErrorCode=%lu (or success at self-send?)\n",
				__func__, L4_IpcFailed(tag) ? L4_ErrorCode() : 0);
			warned = true;
		}
	} while(L4_IpcSucceeded(tag) || L4_ErrorCode() != 6);

	errno = EINTR;
	return -1;
}


int kill(int pid, int signum)
{
	int n = __proc_kill(__the_sysinfo->api.proc, pid, signum);
	if(likely(n == 0)) {
		if(pid == getpid()) __sig_bottom();
		return 0;
	} else if(n < 0) {
		errno = -n;
		return -1;
	} else {
		assert(n > 0);
		fprintf(stderr, "%s: can't reach Sneks::Proc; n=%d\n", __func__, n);
		errno = -ENOSYS;
		return -1;
	}
}


int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	uint64_t or = 0, and = ~0ull;
	switch(how) {
		case SIG_BLOCK: or = *set; break;
		case SIG_UNBLOCK: and = ~*set; break;
		case SIG_SETMASK: or = *set; and = 0; break;
		default:
			errno = EINVAL;
			return -1;
	}

	sigset_t foo = 0;
	if(oldset == NULL) oldset = &foo;
	int n = __proc_sigset(__the_sysinfo->api.proc, oldset, 2, or, and);
	if(n != 0) {
		errno = n < 0 ? -n : EIO;	/* TODO: translate properly! */
		return -1;
	}

	if(how == SIG_UNBLOCK || how == SIG_SETMASK) __sig_bottom();

	return 0;
}


int sigpending(sigset_t *set)
{
	int n = __proc_sigset(__the_sysinfo->api.proc, set, 3, 0, ~0ull);
	if(n != 0) {
		/* TODO: translate properly */
		errno = n > 0 ? EIO : -n;
		return -1;
	}

	return 0;
}


int sigsuspend(const sigset_t *mask)
{
	errno = ENOSYS;
	return -1;
}
