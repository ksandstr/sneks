/* C11-style threading shared between the various runtimes. */
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <errno.h>
#include <epoch.h>
#include <ccan/likely/likely.h>
#include <l4/types.h>
#include <l4/syscall.h>
#include <l4/ipc.h>
#include <l4/schedule.h>
#include <sneks/api/proc-defs.h>
#include <sneks/ipc.h>
#include <sneks/thrd.h>

#define MAGIC 0x60a75ec5
#define STACK_SIZE (1u << __thrd_stksize_log2)

struct thrd {
	int magic, retval;
	_Atomic L4_Word_t j; /* join state, three-way 0/1/tid */
	void *stkbase;
};

static struct thrd *thrd_in(void *stack) {
	uintptr_t end = ((uintptr_t)stack | (STACK_SIZE - 1)) + 1;
	return (struct thrd *)(end - ((sizeof(struct thrd) + 63) & ~63));
}

static struct thrd *myself(void) { return thrd_in(&(int){ 0 }); }

inline L4_ThreadId_t tidof_NP(thrd_t t) { return (L4_ThreadId_t){ .raw = t }; }

thrd_t thrd_current(void) { return L4_MyGlobalId().raw; }

int thrd_equal(thrd_t a, thrd_t b) {
	L4_ThreadId_t ta = tidof_NP(a), tb = tidof_NP(b), d;
	if(L4_IsLocalId(ta) == L4_IsLocalId(tb)) return ta.raw == tb.raw;
	else return ta.raw == L4_ExchangeRegisters(tb, 0, 0, 0, 0, 0, L4_nilthread, &d.raw, &d.raw, &d.raw, &d.raw, &d.raw, &d).raw;
}

int thrd_sleep(const struct timespec *dur, struct timespec *rem) {
	/* TODO: 0 on ok, -1 on signal (fill *rem unless NULL) */
	return -2;
}

int __thrd_sched_yield(void) { L4_Yield(); return 0; }
int sched_yield(void) __attribute__((weak, alias("__thrd_sched_yield")));

void thrd_yield(void) { sched_yield(); }

int thrd_detach(thrd_t thrd) {
	return thrd_success; /* NB: fine: userspace exits with main, systemspace is wonky anyway */
}

static noreturn void thread_wrapper(thrd_start_t fn, void *param) {
	L4_Set_UserDefinedHandle(0);
	L4_Set_ExceptionHandler(L4_nilthread);
	thrd_exit((*fn)(param));
}

int thrd_create(thrd_t *ret, thrd_start_t fn, void *arg)
{
	void *stack = aligned_alloc(STACK_SIZE, STACK_SIZE);
	if(stack == NULL) return thrd_nomem;
	L4_ThreadId_t tid;
	int n = __thrd_new(&tid);
	if(n != 0) { free(stack); return n == -ENOMEM ? thrd_nomem : thrd_error; }
	struct thrd *t = thrd_in(stack);
	*t = (struct thrd){ .magic = MAGIC, .stkbase = stack };
	uintptr_t top = ((uintptr_t)t - 16) & ~0xfu;
#ifdef __SSE__
	top += 4; /* TODO: slightly magical, explain why */
#endif
	L4_Word_t *sp = (L4_Word_t *)top;
	*(--sp) = (L4_Word_t)arg;
	*(--sp) = (L4_Word_t)fn;
	*(--sp) = 0x60d5f007; /* some kind of mushroom perhaps */
	L4_Start_SpIp(tid, (L4_Word_t)sp, (L4_Word_t)&thread_wrapper);
	*ret = tid.raw;
	return thrd_success;
}

noreturn void thrd_exit(int retval)
{
	struct thrd *self = myself();
	self->retval = retval;
	__tss_on_exit();
	L4_ThreadId_t joiner = { .raw = atomic_load_explicit(&self->j, memory_order_relaxed) };
	if(joiner.raw != 0 || !atomic_compare_exchange_strong_explicit(&self->j, &joiner.raw, 1, memory_order_acq_rel, memory_order_relaxed)) {
		assert(joiner.raw != 1 && L4_IsLocalId(joiner));
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw); L4_LoadMR(1, retval);
		if(L4_IpcFailed(L4_Send_Timeout(joiner, L4_TimePeriod(1000)))) { /* TODO: use kip->schedprec */
			int n = __thrd_destroy(L4_Myself());
			fprintf(stderr, "%s: __thrd_destroy: n=%d\n", __func__, n);
		}
	}
	for(;;) L4_Sleep(L4_Never);	/* FIXME: this works around mung crashing in ipc.c:776; remove once fixed. */
	L4_Stop(L4_Myself());
	fprintf(stderr, "%s: self-stop didn't take, sleeping forever\n", __func__);
	for(;;) L4_Sleep(L4_Never);
}

int thrd_join(thrd_t thrd, int *res_p)
{
	int eck = e_begin(), resfoo;
	if(res_p == NULL) res_p = &resfoo;
	L4_Word_t sp, foo;
	L4_ThreadId_t r = L4_ExchangeRegisters(tidof(thrd), 0x200, 0, 0, 0, 0, L4_nilthread, &foo, &sp, &foo, &foo, &foo, &(L4_ThreadId_t){ });
	if(L4_IsNilThread(r)) goto gone;
	struct thrd *t = thrd_in((void *)sp);
	if(t->magic != MAGIC) abort();
	L4_Word_t j_old = atomic_load_explicit(&t->j, memory_order_relaxed);
	if(j_old == 0 && atomic_compare_exchange_strong(&t->j, &j_old, L4_MyLocalId().raw)) { /* first passive join */
		L4_MsgTag_t tag;
		do {
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_Receive(tidof(thrd));
		} while(L4_IpcFailed(tag) && (L4_ErrorCode() == 7 || L4_ErrorCode() == 15)); /* interrupted or aborted */
		if(L4_IpcFailed(tag) && L4_ErrorCode() == 5) goto gone; /* non-existing partner */
		if(L4_IpcSucceeded(tag)) {
			L4_Word_t res; L4_StoreMR(1, &res); *res_p = res;
			int n = __thrd_destroy(tidof(thrd));
			if(n != 0 && n != -ENOENT) fprintf(stderr, "%s: __thrd_destroy: n=%d\n", __func__, n);
		} else {
			*res_p = t->retval;
		}
	} else if(j_old != 1) { /* sloppy seconds */
		*res_p = t->retval;
		while(wait_until_gone(tidof(thrd), L4_Never) != 0) { /* TODO: report out-of-band reception etc. */ }
		goto gone;
	} else { /* active join */
		*res_p = t->retval;
		if(__thrd_destroy(tidof(thrd)) != 0) goto gone;
	}
	e_free(t->stkbase);
	e_end(eck);
	return thrd_success;
gone: e_end(eck); return thrd_error;
}

void __thrd_init(void) {
	struct thrd *self = thrd_in(&(int){ 0 });
	*self = (struct thrd){ .magic = MAGIC };
}
