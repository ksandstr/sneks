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
#include <l4/thread.h>
#include <l4/syscall.h>
#include <l4/ipc.h>
#include <l4/schedule.h>
#include <sneks/ipc.h>
#include <sneks/thrd.h>
#include <sneks/api/proc-defs.h>

#define MAGIC 0x60a75ec5
#define WAIT_IN_USE 0x70075117
#define STACK_SIZE (1u << __thrd_stksize_log2)

struct thrd {
	int magic, retval, err_no;
	void *stkbase, *tss;
	_Atomic L4_Word_t j __attribute__((aligned(64))); /* join state, three-way 0/1/tid */
	struct thrd_wait w;
};

static struct thrd *thrd_of(thrd_t thrd) {
	L4_Word_t _, udh;
	L4_ThreadId_t ret = L4_ExchangeRegisters(tidof(thrd), 0x200, 0, 0, 0, 0, L4_nilthread, &_, &_, &_, &_, &udh, &(L4_ThreadId_t){ });
	return L4_IsNilThread(ret) ? NULL : (struct thrd *)udh;
}

static inline struct thrd *myself(void) { return (struct thrd *)L4_UserDefinedHandle(); }

int *__errno_location(void) { return &myself()->err_no; }

void *__thrd_get_tss(void) { return myself()->tss; }

void __thrd_set_tss(void *ptr) { myself()->tss = ptr; }

struct thrd_wait *__thrd_get_wait(void)
{
#ifndef NDEBUG
	assert(myself()->retval != WAIT_IN_USE);
	myself()->retval = WAIT_IN_USE;
#endif
	assert(myself()->w.tid.raw == L4_MyLocalId().raw);
	return &myself()->w;
}

void __thrd_put_wait(struct thrd_wait *w)
{
#ifndef NDEBUG
	assert(myself()->retval == WAIT_IN_USE);
	myself()->retval = ~WAIT_IN_USE;
#endif
	assert(w == &myself()->w && w->tid.raw == L4_MyLocalId().raw);
}

inline L4_ThreadId_t tidof_NP(thrd_t t) { return (L4_ThreadId_t){ .raw = t }; }

thrd_t thrd_current(void) { return L4_MyGlobalId().raw; }

int thrd_equal(thrd_t a, thrd_t b) { return a == b; }

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

static noreturn void thread_fn(thrd_start_t fn, void *param) {
	assert(myself()->magic == MAGIC);
	L4_Set_ExceptionHandler(L4_nilthread); /* TODO: catch fp etc. booboos */
	thrd_exit((*fn)(param));
}

/* combined Set_UserDefinedHandleOf and Start_SpIp (flags=huisSR,¬H),
 * breaking any ongoing IPC and unhalting w/ @sp and @ip set.
 */
static L4_ThreadId_t start_spip_udh(L4_ThreadId_t t, L4_Word_t sp, L4_Word_t ip, L4_Word_t udh) {
	return L4_ExchangeRegisters(t, 0x15e, sp, ip, 0, udh, L4_nilthread, &sp, &sp, &sp, &sp, &sp, &t);
}

int thrd_create(thrd_t *ret, thrd_start_t fn, void *arg)
{
	void *stack = aligned_alloc(1 << 12, STACK_SIZE);
	if(stack == NULL) return thrd_nomem;
	L4_ThreadId_t tid;
	int n = __thrd_new(&tid); assert(n != 0 || L4_IsGlobalId(tid));
	if(n != 0) { free(stack); return n == -ENOMEM ? thrd_nomem : thrd_error; }
	struct thrd *t = (void *)(((uintptr_t)stack + STACK_SIZE - sizeof *t) & ~0x3ful);
	*t = (struct thrd){ .magic = MAGIC, .stkbase = stack };
	uintptr_t top = (uintptr_t)t - 0x10;
#ifdef __SSE__
	top += 4; /* TODO: slightly magical, explain why */
#endif
	L4_Word_t *sp = (L4_Word_t *)top;
	*(--sp) = (L4_Word_t)arg;
	*(--sp) = (L4_Word_t)fn;
	*(--sp) = 0xd0657055; /* woof */
	t->w.tid = start_spip_udh(tid, (L4_Word_t)sp, (L4_Word_t)&thread_fn, (L4_Word_t)t);
	*ret = tid.raw;
	return thrd_success;
}

noreturn void thrd_exit(int retval)
{
	struct thrd *self = myself();
	self->retval = retval;
	if(self->tss != NULL) { void *tss = self->tss; self->tss = NULL; __tss_on_exit(tss); }
	L4_ThreadId_t joiner = { .raw = atomic_load(&self->j) };
	if(joiner.raw != 0 || !atomic_compare_exchange_strong(&self->j, &joiner.raw, 1)) {
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
	struct thrd *t = thrd_of(thrd); if(t == NULL || unlikely(t->magic != MAGIC)) goto gone;
	L4_Word_t j_old = atomic_load(&t->j);
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
	if(t->stkbase != NULL) e_free(t->stkbase);
	e_end(eck);
	return thrd_success;
gone: e_end(eck); return thrd_error;
}

void __thrd_init(void) {
	static struct thrd first_thread;
	assert(first_thread.magic != MAGIC);
	first_thread = (struct thrd){ .magic = MAGIC, .w.tid = L4_MyLocalId() };
	L4_Set_UserDefinedHandle((L4_Word_t)&first_thread);
}
