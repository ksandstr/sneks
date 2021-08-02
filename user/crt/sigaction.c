
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/sysinfo.h>
#include <sneks/signal.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


#define SIGQBITS 4
#define QSBLKMAX ((1 << SIGQBITS) - 1)


/* "queued signals' block", simple mode. should change heavily once siginfo_t
 * reception comes along.
 */
struct qsblk {
	_Atomic uint32_t rtsigs;
};


typedef void (*handler_bottom_fn)(void);


static void *sig_delivery_page = NULL;
static uint64_t ign_set = 0, dfl_set = ~0ull, block_set = 0;
static struct sigaction sig_actions[64];
static _Atomic bool recv_break_flag = false;

/* process-wide signal queue.
 *
 * sigq_status is split into fields indicating location of head and tail
 * within sigq[], so as to allocate qsblk at the tail and consume at the head.
 * headptr is the first SIGQBITS, tailptr is the next SIGQBITS. QSBLKMAX may
 * be used to mask them off. when headptr=tailptr, insertion occurs at the
 * currently active head (such as when empty). the high 32 bits are a mask of
 * the POSIX reliable (non-realtime) signals.
 */
static struct qsblk sigq[QSBLKMAX + 1];
static _Atomic uint64_t sigq_status = 0;

/* per-thread state (TODO: move into per-thread segment when available) */
static jmp_buf *signal_jmp = NULL;
static ucontext_t *signal_uctx = NULL;


void __permit_recv_interrupt(void)
{
	bool old = atomic_exchange_explicit(&recv_break_flag, true,
		memory_order_acq_rel);
	assert(!old);
}


void __forbid_recv_interrupt(void)
{
	bool old = atomic_exchange_explicit(&recv_break_flag, false,
		memory_order_acq_rel);
	assert(old);
}


/* fetch next signal from sigqueue. returns signums of reliable signals first,
 * and thereafter realtime signals in ascending order per level, advancing
 * headptr as zeroes are encountered, or 0 when the queue is empty.
 *
 * TODO: change the rtsig stuff to proceed in signal order for hypothetical
 * L1i advantage, and POSIX compliance.
 */
static int next_signal(void)
{
	int got;
	uint64_t oldst = atomic_load_explicit(&sigq_status,
		memory_order_relaxed), newst;
	do {
		got = ffsl(oldst >> 32);
		if(got > 0) {
			/* caught a regular POSIX reliable signal. */
			newst = oldst & ~(1ull << (got + 31));
			assert(ffsl(newst >> 32) != got);
		} else {
			/* dequeue a realtime signal. */
			int headptr = oldst & QSBLKMAX,
				tailptr = (oldst >> SIGQBITS) & QSBLKMAX;
			/* find a subqueue that's not empty, and load its mask. */
			uint32_t sigs;
			while(sigs = atomic_load_explicit(
					&sigq[headptr].rtsigs, memory_order_relaxed),
				sigs == 0)
			{
				if(headptr == tailptr) return 0;	/* empty */
				headptr = (headptr + 1) & QSBLKMAX;
			}
			/* take one bit. */
			do {
				got = ffsl(sigs);
			} while(got > 0
				&& !atomic_compare_exchange_strong_explicit(
					&sigq[headptr].rtsigs, &sigs, sigs & ~(1ul << (got - 1)),
					memory_order_relaxed, memory_order_relaxed));
			if(got == 0) {
				/* reload sigq_status to avoid consuming per stale headptr, which
				 * may occur when the asynchronous signal handling mechanism is
				 * interrupted with another signal.
				 */
				oldst = atomic_load_explicit(&sigq_status, memory_order_relaxed);
			} else {
				got += __SIGRTMIN - 1;
				assert((oldst >> 32) == 0);
				newst = headptr | tailptr << SIGQBITS;
			}
		}
	} while(got == 0 || (newst != oldst
		&& !atomic_compare_exchange_strong_explicit(
			&sigq_status, &oldst, newst,
			memory_order_release, memory_order_relaxed)));
	return got;
}


/* discards an indeterminate fraction of input @sigs on -EAGAIN.
 *
 * prefers to add realtime signals to the queue tail, even if they'd fit (all
 * at once or eventually) in earlier slots also. it could be changed to start
 * from headptr and work its way towards headptr-1, and then update tailptr to
 * allow next_signal() to detect an empty queue; this could even be optimized
 * with some sort of a map that tells us how deep the signals are stacked. we
 * could even store some kind of a "turns off" bit instead of 1 for stored, 0
 * for clear.
 *
 * however, all of that is a bit too precious while the current implementation
 * remains mildly fuckered; for now, if one repeated signal ends up consuming
 * all the queue space, then at least one of every other signal can be added,
 * and that seems like it could well be enough.
 */
static int queue_signals(uint64_t sigs)
{
	uint64_t oldst = atomic_load_explicit(&sigq_status,
		memory_order_relaxed), newst;
	do {
		newst = ((oldst >> 32) | (sigs & 0xffffffff)) << 32;

		uint32_t rtsigs = sigs >> 32;
		int headptr = oldst & QSBLKMAX,
			tailptr = (oldst >> SIGQBITS) & QSBLKMAX;
		while(rtsigs &= atomic_fetch_or_explicit(
				&sigq[tailptr].rtsigs, rtsigs, memory_order_relaxed),
			rtsigs != 0)
		{
			/* advance tailptr to deposit again. */
			tailptr = (tailptr + 1) & QSBLKMAX;
			if(tailptr == headptr) return -EAGAIN; /* can't */
		}
		newst |= headptr | tailptr << SIGQBITS;
	} while(newst != oldst
		&& !atomic_compare_exchange_strong_explicit(
			&sigq_status, &oldst, newst,
			memory_order_release, memory_order_relaxed));
	return 0;
}


void __sig_bottom(void)
{
	extern void __invoke_sig_slow(), __invoke_sig_fast();

	const bool recv_break_ok = atomic_exchange_explicit(
		&recv_break_flag, false, memory_order_relaxed);

#if 0
	printf("%s: called in tid=%lu:%lu!\n", __func__,
		L4_ThreadNo(L4_MyGlobalId()), L4_Version(L4_MyGlobalId()));
#endif

	/* get pending signals, or cause async invocation threadlet exit where
	 * applicable.
	 */
	uint64_t pending;
	int call = 0,
		n = __proc_sigset(__the_sysinfo->api.proc, &pending, 4, 0, 0);
	if(n != 0) {
		printf("%s: Proc::sigset failed, n=%d\n", __func__, n);
		/* spin */
		goto noinvoke;
	}
	assert((pending & ign_set) == 0);
	assert((pending & dfl_set) == 0);

	/* __sig_bottom() runs from either the signal invocation helper threadlet,
	 * or from a synchronous signal delivery callsite (such as kill(2) or
	 * sigprocmask(2)). the former may start while the latter is executing, so
	 * we attempt to make recursion safe with a three-state "please call
	 * sigset again" atomic counter.
	 *
	 * TODO: this would be per-thread in a threaded crt.
	 * TODO: and i wonder what this'll do when the handler forks. oh boy.
	 */
	static _Atomic int n_calls = 0;
	int sig = 0;
	call = atomic_fetch_add_explicit(&n_calls, 1, memory_order_relaxed);
	if(call == 0 && (pending & ~block_set)) {
		/* process at most one directly. */
		sig = ffsll(pending & ~block_set) - 1;
		uint64_t bit = 1ull << sig;
		assert(pending & bit);
		assert(~block_set & bit);
		pending &= ~bit;
	}
	/* put rest in queue. */
	if(pending > 0) {
		n = queue_signals(pending);
		if(unlikely(n != 0)) {
			fprintf(stderr, "warning: queue_signals(%#lx) failed, n=%d\n",
				(unsigned long)pending, n);
		}
	}
	if(sig == 0) goto noinvoke;

	int inner;
	const bool in_main = L4_SameThreads(L4_Myself(), __main_tid);
	if(in_main) {
		/* called from outside the invocation threadlet, so that e.g. kill(2)
		 * returns only after the handler has run (unless blocked), and
		 * similar for sigprocmask(2) that unmasks pending signals. permit
		 * threadlet invocation and invoke handler synchronously.
		 */
		assert(call == 0);
		inner = atomic_exchange_explicit(&n_calls, 0, memory_order_relaxed);
		__invoke_sig_sync((sig + 1) | (recv_break_ok ? 1 : 0) << 8);
		if(inner > 1) __sig_bottom();
	} else {
		/* H to halt the thread;
		 * S to interrupt a send phase;
		 * [R to interrupt a receive phase] if applicable;
		 * "h" to write the H flag;
		 * "d" to deliver ctl, sp, ip, and flags.
		 */
		L4_Word_t ctl_out, sp_out, ip_out, flags_out, udh_out;
		L4_ThreadId_t pager_out;
		L4_ThreadId_t ret = L4_ExchangeRegisters(__main_tid,
			0x001 | (recv_break_ok ? 0x002 : 0) | 0x004 | 0x100 | 0x200,
			0, 0, 0, 0, L4_nilthread,
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
		/* parameter is a POSIX signal number, and the recv_break_flag value
		 * that __sig_invoke() should restore.
		 */
		*(--sp) = (sig + 1) | (recv_break_ok ? 1 : 0) << 8;
		L4_Word_t new_ip;
		if((~ctl_out & 0x004) || ((ctl_out & 0x002) && !recv_break_ok)) {
			/* in non-breakable receive phase, or no IPC at all. do the slow
			 * sp/ip switch, possibly only after the receive phase returns
			 * from Ipc, and go into a BR/MR-storing slow-arse signal
			 * invocation routine.
			 */
			new_ip = (L4_Word_t)&__invoke_sig_slow;
		} else {
			/* was in IPC, subsequently broken, so MR/BR need not be saved. */
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

end:
		assert(call == 0);
		inner = atomic_exchange_explicit(&n_calls, 0, memory_order_relaxed);
		if(inner > 1) __sig_bottom();
	}
	return;

noinvoke:
	/* undo the recv_break_depth change, since __sig_invoke won't be doing it
	 * for us now.
	 */
	atomic_store_explicit(&recv_break_flag, recv_break_ok,
		memory_order_relaxed);
	goto end;
}


static void setup_delivery_page(void)
{
	for(int i=0; i <= QSBLKMAX; i++) assert(sigq[i].rtsigs == 0);

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
	if(unlikely(signum < 1 || signum > sizeof ign_set * 8)) {
		errno = EINVAL;
		return -1;
	}
	if(unlikely(sig_delivery_page == NULL)) {
		setup_delivery_page();
		assert(sig_delivery_page != NULL);
	}

	if(act->sa_flags & SA_SIGINFO) {
		fprintf(stderr, "%s: SA_SIGINFO not supported (yet)\n", __func__);
		goto Enosys;
	}

	int n;
	uint64_t bit = 1ull << (signum - 1), old;
	assert((ign_set & dfl_set) == 0);
	if((act->sa_handler == SIG_IGN && (~ign_set & bit))
		|| (act->sa_handler == SIG_DFL && (~dfl_set & bit)))
	{
		/* set bit in ign or dfl. this clears the corresponding position in
		 * the other set.
		 */
		n = __proc_sigset(__the_sysinfo->api.proc, &old,
			act->sa_handler == SIG_IGN ? 0 : 1, bit, ~0ull);
		if(n != 0) goto sigsetfail;
		assert(~old & bit);
		if(act->sa_handler == SIG_IGN) {
			dfl_set &= ~bit; ign_set |= bit;
		} else {
			ign_set &= ~bit; dfl_set |= bit;
		}
	} else if(act->sa_handler != SIG_IGN && act->sa_handler != SIG_DFL
		&& ((ign_set | dfl_set) & bit))
	{
		/* clear bit in either ign or dfl, whichever was set. */
		n = __proc_sigset(__the_sysinfo->api.proc, &old,
			(ign_set & bit) != 0 ? 0 : 1, 0, ~bit);
		if(n != 0) goto sigsetfail;
		assert(old & bit);
		ign_set &= ~bit; dfl_set &= ~bit;
	}
	assert((ign_set & dfl_set) == 0);

	/* block all signals while we modify the handler set. */
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


/* handle @sig and drain the signal queue. called in a normal userspace thread
 * via siginvoke.o, which is machine code.
 */
void __attribute__((regparm(3))) __sig_invoke(int sig, ucontext_t *uctx)
{
	/* (of course we're squeezing other things through there.) */
	const bool recv_break_ok = !!(sig & 0x100);
	sig &= 0xff;
	assert(sig > 0);

	/* set signal_uctx so that the first __sig_invoke() re-/sets it and
	 * inner handlers share that pointer.
	 */
	ucontext_t *oldctx = NULL;
	atomic_compare_exchange_strong_explicit(&signal_uctx, &oldctx, uctx,
		memory_order_relaxed, memory_order_relaxed);
	atomic_signal_fence(memory_order_release);

	jmp_buf sigjmpbuf;
	do {
		const struct sigaction *act = &sig_actions[sig - 1];

		/* FIXME: handle these. they'll appear when a dfl/ign signal appears
		 * in a handler's sa_mask and are raised during its execution, so they
		 * can be hit in a test first.
		 */
		assert(act->sa_handler != SIG_IGN);
		assert(act->sa_handler != SIG_DFL);

		/* apply act->sa_mask, reset only `masked' afterward to enable clean
		 * recursion.
		 */
		volatile uint64_t masked;
		uint64_t old = atomic_load(&block_set), eff_mask = __set2mask(&act->sa_mask);
		if(~act->sa_flags & SA_NODEFER) eff_mask |= 1ull << (sig - 1);
		do {
			masked = eff_mask & ~old;
		} while(!atomic_compare_exchange_strong_explicit(
			&block_set, &old, old | eff_mask,
			memory_order_relaxed, memory_order_relaxed));
		atomic_signal_fence(memory_order_release);

		jmp_buf *volatile old_jmp;
		if(setjmp(sigjmpbuf) == 0) {
			/* set signal_jmp recursively. */
			old_jmp = atomic_exchange_explicit(&signal_jmp, &sigjmpbuf,
				memory_order_relaxed);
			atomic_signal_fence(memory_order_release);
			if(act->sa_flags & SA_SIGINFO) {
				/* FIXME */
				fprintf(stderr,
					"%s: sig=%d specifies SA_SIGINFO, which we don't handle\n",
					__func__, sig);
			} else {
				(*act->sa_handler)(sig);
			}
		} else {
			/* signal handler exited by longjmp(), which ended up in uctx.
			 * proceed to the next signal before reactivating it.
			 */
			atomic_signal_fence(memory_order_acquire);
		}

		jmp_buf *my_jmp = &sigjmpbuf;
		bool ok = atomic_compare_exchange_strong_explicit(
			&signal_jmp, &my_jmp, old_jmp,
			memory_order_relaxed, memory_order_relaxed);
		assert(ok);

		/* TODO: do something about SA_RESTART */
		/* FIXME: restore SIG_DFL disposition if SA_RESETHAND set */

		/* clear block_mask bits previously set per act->sa_mask */
		old = atomic_load(&block_set);
		do {
			assert((old & masked) == masked);
		} while(!atomic_compare_exchange_strong_explicit(
			&block_set, &old, old & ~masked,
			memory_order_relaxed, memory_order_relaxed));
	} while(sig = next_signal(), sig != 0);

	if(oldctx == NULL) {
		oldctx = uctx;
		atomic_compare_exchange_strong_explicit(&signal_uctx, &oldctx, NULL,
			memory_order_relaxed, memory_order_relaxed);
	}

	int old = atomic_exchange_explicit(&recv_break_flag,
		recv_break_ok, memory_order_relaxed);
	assert(old == 0);
}


noreturn void longjmp(jmp_buf env, int val)
{
	if(val == 0) val = 1;
	if(signal_jmp != NULL) {
		assert(signal_uctx != NULL);
		mcontext_t *m = &signal_uctx->mcontext;
		m->eax = val;
		m->ebx = env->regs[0];
		m->esi = env->regs[1];
		m->edi = env->regs[2];
		m->ebp = env->regs[3];
		m->eip = env->regs[4];
		m->esp = env->regs[5] + 4;
		__longjmp_actual(*signal_jmp, 1);
	} else {
		__longjmp_actual(env, val);
	}
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
