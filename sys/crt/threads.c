
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <ccan/likely/likely.h>
#include <ccan/darray/darray.h>
#include <ccan/list/list.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/thread.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include "proc-defs.h"
#include "info-defs.h"


#define SYSCRT_THREAD_MAGIC 0xbea7deaf	/* not a drummer */

typedef darray(void *) tssdata;


L4_ThreadId_t __uapi_tid;

static once_flag init_once = ONCE_FLAG_INIT;
static atomic_flag tss_meta_lock = ATOMIC_FLAG_INIT;
static darray(tss_dtor_t) tss_meta = darray_new();


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


static void thrd_init(void)
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


/* this is basically a spinlock around @func, ensuring that concurrent callers
 * return only once the function has completed.
 */
void call_once(once_flag *flag, void (*func)(void))
{
	int old = atomic_load_explicit(flag, memory_order_relaxed);

again:
	if(likely(old > 1)) {
		/* early out. */
		return;
	} else if(old == 0) {
		/* try to run @func. */
		bool run = atomic_compare_exchange_strong(flag, &old, 1);
		if(!run) goto again;	/* nope! */
		(*func)();
		atomic_store(flag, 2);
	} else if(unlikely(old == 1)) {
		/* wait until concurrent @func completes. */
		while(atomic_load(flag) <= 1) {
			asm volatile ("pause");
		}
	}
}


/* tss_*() family. tss_t is a 1-origin index into tss_meta, so that 0 can stay
 * as the uninitialized value.
 */

static void lock_tss(void) {
	while(!atomic_flag_test_and_set(&tss_meta_lock)) {
		asm volatile ("pause");
	}
}


static inline void unlock_tss(void) {
	atomic_flag_clear(&tss_meta_lock);
}


int tss_create(tss_t *key, void (*dtor)(void *))
{
	lock_tss();
	*key = tss_meta.size + 1;
	darray_push(tss_meta, dtor);
	unlock_tss();

	return 0;
}


void tss_delete(tss_t key)
{
	/* does nothing. should call the dtor for the value in all threads' TSS
	 * segments and then mark @key unused. none of which systasks use.
	 */
}


void *tss_get(tss_t key)
{
	if(unlikely(key <= 0)) return NULL;
	tssdata *data = (tssdata *)L4_UserDefinedHandle();
	return likely(data != NULL && data->size > key - 1)
		? data->item[key - 1] : NULL;
}


void tss_set(tss_t key, void *ptr)
{
	if(unlikely(key <= 0)) return;
	tssdata *data = (tssdata *)L4_UserDefinedHandle();
	if(data == NULL) {
		data = malloc(sizeof *data);
		darray_init(*data);
		L4_Set_UserDefinedHandle((L4_Word_t)data);
	}
	if(data->size <= key - 1) {
		/* NOTE: unsafe access of tss_meta! */
		if(tss_meta.size <= key - 1) {
			printf("!!! %s invalid key=%d (size=%u)\n", __func__,
				key, (unsigned)data->size);
			abort();
		}
		darray_resize0(*data, key);
	}
	data->item[key - 1] = ptr;
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
	call_once(&init_once, &thrd_init);

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
	tssdata *data = (tssdata *)L4_UserDefinedHandle();
	if(data != NULL) {
		lock_tss();
		size_t lim = min(tss_meta.size, data->size);
		tss_dtor_t dtors[lim];
		memcpy(dtors, tss_meta.item, lim);
		unlock_tss();
		for(size_t i=0; i < lim; i++) {
			if(data->item[i] != NULL && dtors[i] != NULL) {
				(*dtors[i])(data->item[i]);
			}
		}
		darray_free(*data);
	}

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
