
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/signal.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


int pause(void)
{
	atomic_signal_fence(memory_order_acq_rel);
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


int kill(pid_t pid, int signum)
{
	int n = __proc_kill(__the_sysinfo->api.proc, pid, signum);
	if(n == 0) {
		if(signum != 0 && pid == getpid()) {
			atomic_signal_fence(memory_order_acq_rel);
			__sig_bottom();
		}
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


int raise(int signum) {
	return kill(getpid(), signum);
}


void abort(void)
{
	/* unblock SIGABRT and raise it. */
	sigset_t abrt_set;
	sigemptyset(&abrt_set);
	sigaddset(&abrt_set, SIGABRT);
	sigprocmask(SIG_UNBLOCK, &abrt_set, NULL);
	raise(SIGABRT);

	/* it was ignored, or caught and the handler returned; restore default
	 * disposition and raise it again.
	 */
	struct sigaction act = { .sa_handler = SIG_DFL };
	sigaction(SIGABRT, &act, NULL);
	sigprocmask(SIG_UNBLOCK, &abrt_set, NULL);
	raise(SIGABRT);

	/* long shot: pop an invalid interrupt to force abnormal kernel entry. */
	for(;;) {
		asm volatile ("int $70");
		L4_Sleep(L4_TimePeriod(10000));
	}
}


int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	uint64_t or = 0, and = ~0ull;
	switch(how) {
		case SIG_BLOCK: or = __set2mask(set); break;
		case SIG_UNBLOCK: and = ~__set2mask(set); break;
		case SIG_SETMASK: or = __set2mask(set); and = 0; break;
		default:
			errno = EINVAL;
			return -1;
	}

	uint64_t old;
	int n = __proc_sigset(__the_sysinfo->api.proc, &old, 2, or, and);
	if(n == 0) {
		if(how == SIG_UNBLOCK || how == SIG_SETMASK) {
			atomic_signal_fence(memory_order_acq_rel);
			__sig_bottom();
		}
	}

	if(oldset != NULL) *oldset = __mask2set(old);
	return NTOERR(n);
}


int sigpending(sigset_t *set)
{
	uint64_t pending;
	int n = __proc_sigset(__the_sysinfo->api.proc, &pending, 3, 0, ~0ull);
	*set = __mask2set(pending);
	return NTOERR(n);
}


int sigsuspend(const sigset_t *mask)
{
	__permit_recv_interrupt();
	unsigned short sig;
	int n = __proc_sigsuspend(__the_sysinfo->api.proc, &sig, __set2mask(mask));
	__forbid_recv_interrupt();
	if(n != 0) return NTOERR(n);
	else {
		/* got one signal immediately. */
		__invoke_sig_sync(sig);
		errno = EINTR;
		return -1;
	}
}
