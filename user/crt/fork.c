/* how to fork in userspace.
 *
 * mung's testbench uses the older method of starting a helper thread and popping
 * an exception to it because the exception message captures the 32-bit x86
 * integer context completely. however, on architectures with more registers,
 * L4.X2's exception messages only carry some of the registers back and forth,
 * so that'd not work outside ia32.
 *
 * instead of that, we'll store the processor context in RAM that gets forked
 * just like any other, and restore the context using an accessory stack
 * (allocated on the fork() caller's stack because of restrictions on malloc).
 */

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <ucontext.h>

#include <l4/types.h>
#include <l4/thread.h>

#include <sneks/process.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


pid_t fork(void)
{
	/* TODO: call thread atfork()s */
	/* TODO: mtx_lock(__malloc_lock); et al. runtime locks */
	/* TODO: __thrd_halt_all_NP(); incl. mutex thread etc. */
	/* TODO: generate file descriptor buffers */

	pid_t child_pid;
	volatile L4_ThreadId_t parent_tid = L4_MyGlobalId();
	ucontext_t child_ctx;
	getcontext(&child_ctx);
	if(parent_tid.raw != L4_MyGlobalId().raw) {
		/* CHILD SIDE. */
		child_pid = 0;
	} else {
		/* PARENT SIDE. launch the child process. */
		char tmpstack[128] __attribute__((aligned(16)));
		volatile L4_Word_t *stktop = (L4_Word_t *)&tmpstack[sizeof tmpstack];
		*(--stktop) = (L4_Word_t)&child_ctx;
		*(--stktop) = 0xdeadbeef;	/* faux return address */

		L4_ThreadId_t child_tid;
		int n = __proc_fork(__the_sysinfo->api.proc, &child_tid.raw,
			(L4_Word_t)stktop, (L4_Word_t)&setcontext);
		if(n == 0) child_pid = pidof_NP(child_tid);
		else {
			/* FIXME: set errno */
			fprintf(stderr, "%s: Proc::fork failed, n=%d\n", __func__, n);
			child_pid = -1;
		}
	}

	/* COMMON EPILOG. */
	/* TODO: resume daemon threads */

	return child_pid;
}
