
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "proc-defs.h"
#include "private.h"


typedef void (*handler_bottom_fn)(void);


static void *sig_delivery_page = NULL;
static uint64_t ign_set = 0, dfl_set = ~0ull, block_set = 0;
static struct sigaction sig_actions[64];


void __sig_bottom(void)
{
	extern void __invoke_sig_slow(), __invoke_sig_fast();
#if 0
	printf("%s: called in tid=%lu:%lu!\n", __func__,
		L4_ThreadNo(L4_MyGlobalId()), L4_Version(L4_MyGlobalId()));
#endif
	const bool in_main = L4_SameThreads(L4_Myself(), __main_tid);

	uint64_t pending;
	int n = __proc_sigset(__the_sysinfo->api.proc, &pending, 3, 0, 0);
	if(n != 0) {
		printf("%s: Proc::sigset failed, n=%d\n", __func__, n);
		/* spin */
		return;
	}

	bool update_block = false;
	while(pending > 0) {
		int sig = ffsll(pending) - 1;
		assert((pending & (1ull << sig)) != 0);
		pending &= ~(1ull << sig);

		/* TODO: do this for each thread once multithreading comes about.
		 * also, move block_set into the thread control structure.
		 */
		if((block_set & (1ull << sig)) != 0) {
			// printf("%s: blocked (lazy)\n", __func__);
			update_block = true;
			continue;
		}

		if(in_main) {
			/* called in the thread that'd be exregs'd into sighandler land,
			 * which it can't do to itself. this happens when kill(2) signals
			 * the calling process itself; in that case kill(2) mustn't return
			 * until the handler has run (unless blocked). fortunately
			 * function calls are just as good.
			 */
			__sig_invoke(sig + 1);
			continue;
		}

		/* H to halt the thread;
		 * S to interrupt a send phase;
		 * "h" to write the H flag;
		 * "d" to deliver ctl, sp, ip, and flags.
		 */
		L4_Word_t ctl_out, sp_out, ip_out, flags_out, udh_out;
		L4_ThreadId_t pager_out;
		L4_ThreadId_t ret = L4_ExchangeRegisters(__main_tid,
			0x001 | 0x004 | 0x100 | 0x200, 0, 0, 0, 0, L4_nilthread,
			&ctl_out, &sp_out, &ip_out, &flags_out, &udh_out, &pager_out);
		if(L4_IsNilThread(ret)) {
			printf("%s: send-halt ExchangeRegisters failed, ec=%lu\n",
				__func__, L4_ErrorCode());
			/* FIXME: do something else? */
			abort();
		}

		assert((ctl_out & 0x001) == 0);	/* must not have been halted. */
		L4_Word_t *sp = (void *)sp_out;
		*(--sp) = ip_out;	/* return address */
		*(--sp) = flags_out;
		*(--sp) = sig + 1;	/* POSIX signal number */
		/* TODO: interrupt an interruptable receive phase (where declared
		 * explicitly, such as waitid(2) or select(2) or any of the blocking
		 * I/O calls), and use the __invoke_sig_fast path.
		 */
		L4_Word_t new_ip;
		if((ctl_out & 0x002) != 0 || (ctl_out & 0x004) == 0) {
			/* in receive phase, or no IPC at all. do the slow sp/ip switch,
			 * possibly only after the receive phase returns from Ipc, and go
			 * into a BR/MR-storing slow-arse signal invocation routine.
			 */
			new_ip = (L4_Word_t)&__invoke_sig_slow;
		} else {
			/* was in a send-phase that was aborted. nice! */
			new_ip = (L4_Word_t)&__invoke_sig_fast;
		}
		/* TODO: block @sig for handler to clear */
		/* ¬H to resume the thread;
		 * "i" to write ip;
		 * "s" to write sp;
		 * "h" to write the H flag;
		 */
		L4_Word_t dummy;
		L4_ThreadId_t dummy_tid;
		ret = L4_ExchangeRegisters(__main_tid, 0x008 | 0x010 | 0x100,
			(L4_Word_t)sp, new_ip, 0, 0, L4_nilthread,
			&ctl_out, &dummy, &dummy, &dummy, &dummy, &dummy_tid);
		if(L4_IsNilThread(ret)) {
			printf("%s: sigfast exregs failed, ec=%lu\n", __func__,
				L4_ErrorCode());
			/* FIXME: respond somehow */
			abort();
		}
		assert((ctl_out & 0x001) != 0);	/* must have been halted. */
	}

	if(update_block) {
		uint64_t old;
		n = __proc_sigset(__the_sysinfo->api.proc, &old, 2, block_set, 0);
		/* return status doesn't matter; this is an optimization anyway. */
	}
}


static void setup_delivery_page(void)
{
	int pagesz = sysconf(_SC_PAGESIZE);
	sig_delivery_page = aligned_alloc(pagesz, pagesz);
	if(sig_delivery_page == NULL) {
		fprintf(stderr, "%s: can't allocate %d aligned bytes (wtf?)\n",
			__func__, pagesz);
		abort();
	}

	unsigned tail_len = 1024;
	uint8_t tailbuf[tail_len];
	int32_t handler_offset = -1;
	int n = __proc_sigconfig(__the_sysinfo->api.proc,
		(L4_Word_t)sig_delivery_page, tailbuf, &tail_len,
		&handler_offset);
	if(n != 0) {
		/* FIXME: retry on transfer timeout, which might happen under
		 * memory pressure so severe that even the stack has been swapped
		 * out. or an otherwise fucky pager.
		 */
		fprintf(stderr, "%s: Proc::sigsetup failed, n=%d\n", __func__, n);
		abort();
	}

	memset(sig_delivery_page, '\0', pagesz - sizeof tailbuf);
	memcpy(sig_delivery_page + pagesz - sizeof tailbuf, tailbuf,
		max_t(unsigned, tail_len, sizeof tailbuf));
	if(handler_offset < 0
		|| handler_offset > pagesz - sizeof(handler_bottom_fn))
	{
		fprintf(stderr, "%s: Proc::sigsetup returned handler_offset=%d\n",
			__func__, handler_offset);
		abort();
	}
	*((handler_bottom_fn *)(sig_delivery_page + handler_offset)) = &__sig_bottom;
}


int sigaction(
	int signum,
	const struct sigaction *act,
	struct sigaction *oldact)
{
	if(unlikely(sig_delivery_page == NULL)) {
		setup_delivery_page();
		assert(sig_delivery_page != NULL);
	}

	if((act->sa_flags & SA_SIGINFO) != 0) {
		fprintf(stderr, "%s: SA_SIGINFO not supported (yet)\n", __func__);
		goto Enosys;
	}

	int n;
	uint64_t bit = 1ull << (signum - 1), old;
	assert((ign_set & dfl_set) == 0);
	if((act->sa_handler == SIG_IGN && (ign_set & bit) == 0)
		|| (act->sa_handler == SIG_DFL && (dfl_set & bit) == 0))
	{
		/* set bit in ign or dfl. this clears the corresponding position in
		 * the other set.
		 */
		n = __proc_sigset(__the_sysinfo->api.proc, &old,
			act->sa_handler == SIG_IGN ? 0 : 1, bit, ~0ull);
		if(n != 0) goto sigsetfail;
		assert((old & bit) == 0);
		if(act->sa_handler == SIG_IGN) {
			dfl_set &= ~bit; ign_set |= bit;
		} else {
			ign_set &= ~bit; dfl_set |= bit;
		}
	} else if(((ign_set | dfl_set) & bit) != 0) {
		/* clear bit in either ign or dfl, whichever was set. */
		n = __proc_sigset(__the_sysinfo->api.proc, &old,
			(ign_set & bit) != 0 ? 0 : 1, 0, ~bit);
		if(n != 0) goto sigsetfail;
		assert((old & bit) != 0);
		ign_set &= ~bit; dfl_set &= ~bit;
	}
	assert((ign_set & dfl_set) == 0);

	/* block all signals while we modify the handler set.
	 *
	 * TODO: under multithreading, take the handler set's mutex and block
	 * signal processing for all threads over this part.
	 */
	old = atomic_exchange_explicit(&block_set, ~0ull, memory_order_relaxed);
	if(oldact != NULL) *oldact = sig_actions[signum - 1];
	sig_actions[signum - 1] = *act;
	atomic_store_explicit(&block_set, old, memory_order_relaxed);

	return 0;

sigsetfail:
	/* FIXME: handle! */
	fprintf(stderr, "%s: Proc::sigset failed, n=%d\n", __func__, n);
	abort();

Enosys:
	errno = ENOSYS;
	return -1;
}


/* called from siginvoke.o, which is machine code. */
void __attribute__((regparm(3))) __sig_invoke(int sig)
{
	struct sigaction *act = &sig_actions[sig - 1];

	/* apply act->sa_mask, reset `masked' afterward */
	uint64_t masked,
		old = atomic_load_explicit(&block_set, memory_order_relaxed);
	do {
		masked = act->sa_mask ^ old;
	} while(!atomic_compare_exchange_weak_explicit(&block_set, &old,
		old | act->sa_mask, memory_order_relaxed, memory_order_relaxed));

	/* FIXME: unblock @sig if SA_NODEFER set */
	if((act->sa_flags & SA_SIGINFO) != 0) {
		/* FIXME */
		fprintf(stderr,
			"%s: sig=%d specifies SA_SIGINFO, which we don't handle\n",
			__func__, sig);
	} else {
		(*act->sa_handler)(sig);
	}

	/* TODO: do something about SA_RESTART */
	/* FIXME: restore SIG_DFL disposition if SA_RESETHAND set */

	old = atomic_load_explicit(&block_set, memory_order_relaxed);
	do {
		assert((old & masked) == masked);
	} while(!atomic_compare_exchange_weak_explicit(&block_set, &old,
		old & ~masked, memory_order_relaxed, memory_order_relaxed));
}


sighandler_t signal(int signum, sighandler_t handler)
{
	/* BSD semantics, please. just like glibc 2 and later. */
	struct sigaction old, act = {
		.sa_handler = handler,
		.sa_flags = SA_RESTART,
	};
	int n = sigaction(signum, &act, &old);
	return n < 0 ? SIG_ERR : old.sa_handler;
}