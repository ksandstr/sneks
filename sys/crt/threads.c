
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <ccan/likely/likely.h>
#include <ccan/list/list.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/thread.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include "proc-defs.h"
#include "info-defs.h"
#include "private.h"


#define SYSCRT_THREAD_MAGIC 0xbea7deaf	/* not a drummer */


L4_ThreadId_t __uapi_tid = L4_anythread;


static void thread_ctor(struct thrd *t, L4_ThreadId_t tid)
{
	*t = (struct thrd){
		.magic = SYSCRT_THREAD_MAGIC,
		.alive = true, .tid = tid,
	};
}


static struct thrd *thrd_in_stack(void *stkptr)
{
	uintptr_t base = (uintptr_t)stkptr & ~(STKSIZE - 1),
		ctx = (base + STKSIZE - sizeof(struct thrd)) & ~0x3f;
	return (struct thrd *)ctx;
}


void __thrd_init(void)
{
	struct thrd *t = thrd_in_stack(&t);
	thread_ctor(t, L4_Myself());

	struct sneks_uapi_info ui;
	int n = __info_uapi_block(L4_Pager(), &ui);
	if(n != 0) {
		fprintf(stderr, "%s: can't get UAPI info block, n=%d\n", __func__, n);
		abort();
	}
	__uapi_tid.raw = ui.service;
	assert(!L4_IsNilThread(__uapi_tid));
	assert(L4_IsGlobalId(__uapi_tid));
}


struct thrd *thrd_from_tid(L4_ThreadId_t tid)
{
	if(L4_IsNilThread(tid)) return NULL;

	/* read thread stack pointer. */
	L4_Word_t dummy, sp = 0;
	L4_ThreadId_t dummy_tid, tid_out;
	tid_out = L4_ExchangeRegisters(tid, 1 << 9, /* d-eliver */
		0, 0, 0, 0, L4_nilthread, &dummy, &sp, &dummy, &dummy, &dummy,
		&dummy_tid);
	if(L4_IsNilThread(tid_out)) return NULL;
	assert(sp >= 0x10000);
	struct thrd *t = thrd_in_stack((void *)sp);
	if(t->magic != SYSCRT_THREAD_MAGIC) t = NULL;
	return t;
}


L4_ThreadId_t thrd_to_tid(thrd_t thr)
{
	assert(sizeof(thrd_t) == sizeof(struct thrd *));
	struct thrd *t = (struct thrd *)thr;
	assert(t->magic == SYSCRT_THREAD_MAGIC);
	return t->tid;
}


/* threads */

thrd_t thrd_current(void) {
	int foo;
	return (thrd_t)thrd_in_stack(&foo);
}


static void wrapper_fn(thrd_start_t fn, void *ptr) {
	thrd_exit((*fn)(ptr));
}


int thrd_create(thrd_t *thread, thrd_start_t fn, void *param_ptr)
{
	void *stkbase = aligned_alloc(STKSIZE, STKSIZE);
	if(stkbase == NULL) return thrd_nomem;
	struct thrd *t = thrd_in_stack(stkbase);
	thread_ctor(t, L4_nilthread);

	int n = __proc_create_thread(__uapi_tid, &t->tid.raw);
	if(n != 0) {
		fprintf(stderr, "%s: Proc::create_thread failed, n=%d\n", __func__, n);
		free(stkbase);
		return thrd_error;
	}

	uintptr_t top = ((L4_Word_t)t - 16) & ~0xfu;
#ifdef __SSE__
	top += 4;
#endif
	L4_Word_t *sp = (L4_Word_t *)top;
	*(--sp) = (L4_Word_t)param_ptr;
	*(--sp) = (L4_Word_t)fn;
	*(--sp) = 0xfeedf007;	/* why else would it be in your mouth? */
	L4_Start_SpIp(t->tid, (L4_Word_t)sp, (L4_Word_t)&wrapper_fn);

	*thread = (thrd_t)t;
	return thrd_success;
}


/* FIXME: the join-exit IPC protocol has no affordance for error handling. it
 * should at least return thrd_error and put the system in some reasonable
 * state.
 */
void thrd_exit(int res)
{
	/* destroy per-thread data. this access pattern wrt tss_meta is mildly bad
	 * because of the spinlock section.
	 */
	extern void __tss_on_exit(void);
	__tss_on_exit();

	L4_Set_UserDefinedHandle(res);

	struct thrd *t = thrd_in_stack(&res);
	assert(t->magic == SYSCRT_THREAD_MAGIC);
	/* set up for passive exit. */
	atomic_store(&t->res, res);
	atomic_store(&t->alive, false);
	struct thrd *joiner = atomic_load(&t->joiner);
	if(joiner != NULL) {
		/* active exit happened instead. */
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
		L4_LoadMR(1, res);
		L4_Send(joiner->tid);
		__proc_remove_thread(__uapi_tid, L4_Myself().raw, L4_MyLocalId().raw);
	} else {
		/* (other side calls Proc::remove_thread.) */
	}

	/* this halts the thread. (but loop around it just in case.) */
	for(;;) {
		L4_Sleep(L4_Never);
		L4_Set_ExceptionHandler(L4_nilthread);
		asm volatile ("int $0xd0");	/* die die die */
	}
}


int thrd_join(thrd_t thr, int *res_p)
{
	struct thrd *t = (struct thrd *)thr;
	if(t->magic != SYSCRT_THREAD_MAGIC || t->joiner != NULL) {
		return thrd_error;
	}

	L4_ThreadId_t tid = t->tid;
	uintptr_t stkbase = (uintptr_t)t & ~(STKSIZE - 1);
	if(!atomic_load(&t->alive)) {
active:
		/* active join. */
		if(res_p != NULL) *res_p = t->res;
		free((void *)stkbase);
		int n = __proc_remove_thread(__uapi_tid, tid.raw, L4_LocalIdOf(tid).raw);
		return n == 0 ? thrd_success : thrd_error;
	} else {
		/* passive join. */
		int dummy;
		atomic_store(&t->joiner, thrd_in_stack(&dummy));
		if(!atomic_load(&t->alive)) goto active;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Receive(tid);
		if(L4_IpcFailed(tag)) return thrd_error;
		else {
			L4_Word_t resw; L4_StoreMR(1, &resw);
			if(res_p != NULL) *res_p = resw;
			free((void *)stkbase);
			return thrd_success;
		}
	}
}
