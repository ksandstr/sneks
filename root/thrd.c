
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <assert.h>

#include <ccan/likely/likely.h>
#include <ccan/compiler/compiler.h>

#include <l4/types.h>
#include <l4/syscall.h>
#include <l4/ipc.h>
#include <l4/kip.h>

#include <sneks/test.h>
#include <sneks/systask.h>
#include <sneks/api/proc-defs.h>

#include "defs.h"
#include "epoch.h"


#define RT_THREAD_MAGIC 0xb007fade	/* un-shine */


/* thread context stuff for roottask threads. stored at top of stack to avoid
 * tracking etc. overhead.
 */
struct rt_thread {
	uint32_t magic;
	_Atomic bool alive;
	_Atomic int retval;
	_Atomic unsigned long joiner_tid;
	int max_tss;
};


static tss_t epoch_tss;

int next_early_utcb_slot = 1;


L4_ThreadId_t thrd_tidof_NP(thrd_t t) {
	return (L4_ThreadId_t){ .raw = t };
}


static void rt_thread_ctor(struct rt_thread *t)
{
	*t = (struct rt_thread){
		.magic = RT_THREAD_MAGIC, .alive = true,
		.joiner_tid = L4_nilthread.raw,
		.retval = 0, .max_tss = -1,
	};
}


static struct rt_thread *rt_thread_in(void *stack)
{
	uintptr_t stktop = (uintptr_t)stack | (THREAD_STACK_SIZE - 1);
	int res = (sizeof(struct rt_thread) + 63) & ~63;
	struct rt_thread *t = (void *)(stktop - res + 1);
	return t;
}


static struct rt_thread *rt_self(void)
{
	int dummy;
	struct rt_thread *t = rt_thread_in(&dummy);
	assert(t->magic == RT_THREAD_MAGIC);
	return t;
}


noreturn void thrd_exit(int res)
{
	struct rt_thread *rt = rt_self();
	atomic_store(&rt->retval, res);
	atomic_store(&rt->alive, false);

	extern void __tss_on_exit(void); /* this'd be alone in a header else. */
	__tss_on_exit();

	L4_ThreadId_t joiner = { .raw = atomic_load(&rt->joiner_tid) };
	if(!L4_IsNilThread(joiner)) {
		L4_Set_ExceptionHandler(joiner);
		for(;;) asm volatile ("int $69" :: "a" (res));
	} else {
		/* soft halt. */
#if 0
		L4_Stop(L4_Myself());
		printf("%s: self-stop didn't take\n", __func__);
#endif
		for(;;) L4_Sleep(L4_Never);
	}
}


static void thread_wrapper(L4_Word_t fn, L4_Word_t param)
{
	/* TODO: do something useful with these. TSD and reporting of math faults,
	 * for example.
	 */
	L4_Set_UserDefinedHandle(0);
	L4_Set_ExceptionHandler(L4_nilthread);

	int retval = (*(thrd_start_t)fn)((void *)param);
	thrd_exit(retval);
}


/* this may be called before uapi_init(). at that point it'll create threads
 * in the forbidden range for e.g. the sysmem early pager. after uapi_init()
 * it'll use the proper roottask range.
 */
int thrd_create(thrd_t *t, thrd_start_t fn, void *param_ptr)
{
	L4_ThreadId_t tid;

	if(L4_IsNilThread(uapi_tid)) {
		static L4_Word_t utcb_base;
		static int next_tid;
		static bool first = true;
		if(unlikely(first)) {
			int u_align = 1 << L4_UtcbAlignmentLog2(the_kip);
			utcb_base = L4_MyLocalId().raw & ~(u_align - 1);
			next_tid = L4_ThreadNo(L4_Myself()) + 1;
			first = false;
		}
		tid = L4_GlobalId(next_tid, L4_Version(L4_Myself()));
		L4_Word_t r = L4_ThreadControl(tid, L4_Myself(), L4_Myself(), L4_Pager(),
			(void *)(utcb_base + next_early_utcb_slot * L4_UtcbSize(the_kip)));
		if(r == 0) {
			printf("%s: threadctl failed, ec=%#lx\n", __func__, L4_ErrorCode());
			return thrd_error;
		}
		next_early_utcb_slot++;
		next_tid++;
	} else {
		int n = __proc_create_thread(uapi_tid, &tid.raw);
		if(n != 0) {
			printf("%s: Proc::create_thread failed, n=%d\n", __func__, n);
			return thrd_error;
		}
	}

	void *stack = aligned_alloc(THREAD_STACK_SIZE, THREAD_STACK_SIZE);
	struct rt_thread *rt = rt_thread_in(stack);
	rt_thread_ctor(rt);
	L4_Word_t stk_top = ((L4_Word_t)rt - 16) & ~0xfu;
#ifdef __SSE__
	/* FIXME: see comment in mung testbench start_thread_long() */
	stk_top += 4;
#endif
	L4_Word_t *sp = (L4_Word_t *)stk_top;
	*(--sp) = (L4_Word_t)param_ptr;
	*(--sp) = (L4_Word_t)fn;
	*(--sp) = 0xdeadb007;
	L4_Start_SpIp(tid, (L4_Word_t)sp, (L4_Word_t)&thread_wrapper);

	*t = tid.raw;
	return thrd_success;
}


int thrd_join(thrd_t thrd, int *res_p)
{
	/* read thread stack pointer. */
	L4_Word_t dummy, sp = 0;
	L4_ThreadId_t dummy_tid;
	L4_ExchangeRegisters(thrd_tidof_NP(thrd), 1 << 9, /* d-eliver */
		0, 0, 0, 0, L4_nilthread, &dummy, &sp, &dummy, &dummy, &dummy,
		&dummy_tid);
	struct rt_thread *rt = rt_thread_in((void *)sp);

again:
	if(atomic_load(&rt->alive)) {
		/* passive join */
		L4_Word_t old = L4_nilthread.raw;
		if(!atomic_compare_exchange_strong(
			&rt->joiner_tid, &old, L4_Myself().raw))
		{
			/* doesn't support more than one joiner at a time. */
			printf("%s: joiner=%#lx\n", __func__, old);
			return thrd_error;
		}
		if(!atomic_load(&rt->alive)) goto again;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Receive(thrd_tidof_NP(thrd));
		if(L4_IpcFailed(tag)) {
			printf("%s: frame receive failed, ec=%lu\n",
				__func__, L4_ErrorCode());
			return thrd_error;
		}
		if(res_p != NULL) {
			L4_Word_t tmp;
			L4_StoreMR(12, &tmp);
			*res_p = tmp;
		}
	} else {
		/* active join */
		if(res_p != NULL) *res_p = rt->retval;
	}

	L4_ThreadId_t t_tid = thrd_tidof_NP(thrd);
	if(!L4_IsNilThread(uapi_tid)) {
		assert(L4_IsGlobalId(t_tid));
		int n = __proc_remove_thread(uapi_tid, t_tid.raw,
			L4_LocalIdOf(t_tid).raw);
		if(n != 0) {
			printf("%s: Proc::remove_thread() failed, n=%d\n",
				__func__, n);
			return thrd_error;
		}
	} else {
		L4_Word_t res = L4_ThreadControl(t_tid, L4_nilthread,
			L4_nilthread, L4_nilthread, (void *)-1);
		if(res != 1) {
			printf("%s: deleting ThreadControl failed, ec=%lu\n",
				__func__, L4_ErrorCode());
			if(L4_ErrorCode() != 2) return thrd_error;
		}
	}

	uintptr_t stkbase = (uintptr_t)rt & ~(THREAD_STACK_SIZE - 1);
	free((void *)stkbase);

	return thrd_success;
}


void *e_ext_get(size_t size, void (*dtor_fn)(void *ptr))
{
	void *ptr = tss_get(epoch_tss);
	if(ptr == NULL) {
		ptr = calloc(1, size);
		tss_set(epoch_tss, ptr);
	}
	return ptr;
}


COLD void rt_thrd_init(void)
{
	int dummy;
	rt_thread_ctor(rt_thread_in(&dummy));

	/* FIXME: this doesn't do what epoch.c specifies; dtors are completely
	 * absent from root's tss stuff.
	 */
	int n = tss_create(&epoch_tss, NULL);
	assert(n == thrd_success);
}


#ifdef BUILD_SELFTEST
#include <sneks/test.h>
#include <sneks/systask.h>

static int return_one_fn(void *param_ptr) {
	return 1;
}


START_TEST(start_and_join)
{
	diag("uapi_tid=%lu:%lu", L4_ThreadNo(uapi_tid), L4_Version(uapi_tid));
	plan(14 + 1);

	/* basic create and join, seven times over. */
	int total = 0;
	for(int i=0; i < 7; i++) {
		thrd_t t;
		int n = thrd_create(&t, &return_one_fn, NULL);
		ok(n == thrd_success, "created thread i=%d", i);
		int res = -1;
		n = thrd_join(t, &res);
		ok(n == thrd_success, "joined thread i=%d", i);
		total += res;
	}
	ok1(total == 7);
}
END_TEST

SYSTASK_SELFTEST("root:thrd", start_and_join);
#endif
