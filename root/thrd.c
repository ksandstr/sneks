/* root's threading. distinct from sys/crt/threads.c in that this does direct
 * ThreadControl calls instead of relying on UAPI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <threads.h>
#include <assert.h>
#include <ccan/htable/htable.h>
#include <l4/types.h>
#include <l4/syscall.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <sneks/hash.h>
#include <sneks/api/proc-defs.h>
#include "defs.h"
#include "epoch.h"

#define MAGIC 0xb00bd06e	/* tity hound */
#define STACK_SIZE 4096

/* thread context at top of stack, aligned to STACK_SIZE. */
struct rt_thread {
	uint32_t magic;
	_Atomic bool alive;
	int retval, max_tss;
	L4_ThreadId_t tid;
	void *stkbase;
};

struct epoch_data {
	void (*dtor)(void *);
	char data[];
};

static size_t rehash_rt_thread(const void *, void *);

static tss_t epoch_tss;
/* TODO: put a spinlock around all_threads */
static struct htable all_threads = HTABLE_INITIALIZER(all_threads, &rehash_rt_thread, NULL);
int next_early_utcb_slot = 1;

static size_t rehash_rt_thread(const void *ptr, void *priv) {
	const struct rt_thread *t = ptr;
	return int_hash(t->tid.raw);
}

static bool cmp_rt_thread_to_tid(const void *cand, void *keyptr) {
	const struct rt_thread *t = cand;
	const L4_ThreadId_t *key = keyptr;
	return t->tid.raw == key->raw;
}

L4_ThreadId_t tidof_NP(thrd_t t) {
	return (L4_ThreadId_t){ .raw = t };
}

static struct rt_thread *rt_thread_in(void *stack) {
	uintptr_t end = ((uintptr_t)stack | (STACK_SIZE - 1)) + 1;
	return (struct rt_thread *)(end - ((sizeof(struct rt_thread) + 63) & ~63));
}

static struct rt_thread *rt_self(void) {
	int dummy;
	assert(rt_thread_in(&dummy)->magic == MAGIC);
	return rt_thread_in(&dummy);
}

noreturn void thrd_exit(int retval)
{
	struct rt_thread *self = rt_self();
	assert(L4_SameThreads(self->tid, L4_Myself()));
	self->retval = retval;
	extern void __tss_on_exit(void); /* this'd be alone in a header else. */
	__tss_on_exit();
	if(!L4_IsNilThread(uapi_tid)) {
		int n = __proc_remove_thread(uapi_tid, self->tid.raw, L4_LocalIdOf(self->tid).raw);
		if(n != 0) printf("%s: Proc::remove_thread failed, n=%d\n", __func__, n);
	} else {
		L4_ThreadControl(self->tid, L4_nilthread, L4_nilthread, L4_nilthread, (void *)-1);
		printf("%s: self-delete didn't take: ec=%#lx\n", __func__, L4_ErrorCode());
	}
	/* soft halt, but prevents thrd_join(). */
	L4_Stop(L4_Myself());
	printf("%s: self-stop didn't take\n", __func__);
	for(;;) L4_Sleep(L4_Never);
}

static noreturn void thread_wrapper(thrd_start_t fn, void *param)
{
	L4_Set_UserDefinedHandle(0);
	L4_Set_ExceptionHandler(L4_nilthread);
	thrd_exit((*fn)(param));
}

int thrd_create(thrd_t *t, thrd_start_t fn, void *param_ptr)
{
	void *stack = aligned_alloc(STACK_SIZE, STACK_SIZE);
	if(stack == NULL) return thrd_nomem;

	L4_ThreadId_t tid;
	if(L4_IsNilThread(uapi_tid)) {
		static L4_Word_t utcb_base;
		static int next_tid;
		static bool first = true;
		if(first) {
			int u_align = 1 << L4_UtcbAlignmentLog2(the_kip);
			utcb_base = L4_MyLocalId().raw & ~(u_align - 1);
			next_tid = L4_ThreadNo(L4_Myself()) + 1;
			first = false;
		}
		/* use the forbidden range before UAPI for boot-up pagers etc. */
		tid = L4_GlobalId(next_tid, L4_Version(L4_Myself()));
		L4_Word_t r = L4_ThreadControl(tid, L4_Myself(), L4_Myself(), L4_Pager(),
			(void *)(utcb_base + next_early_utcb_slot * L4_UtcbSize(the_kip)));
		if(r == 0) {
			printf("%s: threadctl failed, ec=%#lx\n", __func__, L4_ErrorCode());
			free(stack);
			return thrd_error;
		}
		next_early_utcb_slot++;
		next_tid++;
	} else {
		int n = __proc_create_thread(uapi_tid, &tid.raw);
		if(n != 0) {
			printf("%s: Proc::create_thread failed, n=%d\n", __func__, n);
			free(stack);
			return thrd_error;
		}
	}

	struct rt_thread *rt = rt_thread_in(stack);
	*rt = (struct rt_thread){ .magic = MAGIC, .alive = true, .tid = tid, .stkbase = stack };
	if(!htable_add(&all_threads, rehash_rt_thread(rt, NULL), rt)) {
		free(stack);
		/* FIXME: destroy `tid' */
		return thrd_nomem;
	}
	uintptr_t top = ((uintptr_t)rt - 16) & ~0xfu;
#ifdef __SSE__
	/* FIXME: see comment in mung testbench start_thread_long() */
	top += 4;
#endif
	L4_Word_t *sp = (L4_Word_t *)top;
	*(--sp) = (L4_Word_t)param_ptr;
	*(--sp) = (L4_Word_t)fn;
	*(--sp) = 0xdeadb007;	/* but they ain't walking no mo' */
	L4_Start_SpIp(tid, (L4_Word_t)sp, (L4_Word_t)&thread_wrapper);

	*t = tid.raw;
	return thrd_success;
}

int thrd_join(thrd_t thrd, int *res_p)
{
	int eck = e_begin();
	size_t hash = int_hash(tidof_NP(thrd).raw);
	struct rt_thread *t = htable_get(&all_threads, hash, &cmp_rt_thread_to_tid, &(L4_ThreadId_t){ tidof_NP(thrd).raw });
	if(t == NULL || !atomic_load(&t->alive)) {
		e_end(eck);
		return thrd_error;
	}
	bool killer = atomic_exchange(&t->alive, false);
	L4_ThreadId_t tid = t->tid;
	e_end(eck);

	L4_MsgTag_t tag;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_Receive(tid);
		/* TODO: report out-of-band reception */
	} while(L4_IpcSucceeded(tag) || L4_ErrorCode() != 5);
	if(!killer) return thrd_error;

	if(res_p != NULL) *res_p = t->retval;
	htable_del(&all_threads, hash, t);
	e_free(t->stkbase);
	return thrd_success;
}

/* for epoch.c of lfht */
void *e_ext_get(size_t size, void (*dtor_fn)(void *ptr))
{
	struct epoch_data *data = tss_get(epoch_tss);
	if(data == NULL) {
		data = calloc(1, sizeof(struct epoch_data) + size);
		data->dtor = dtor_fn;
		tss_set(epoch_tss, data);
	}
	return data->data;
}

static void epoch_first_dtor(void *ptr) {
	struct epoch_data *data = ptr;
	(*data->dtor)(data->data);
	free(data);
}

void init_root_thrd(void)
{
	int stack_dummy;	/* thicc */
	struct rt_thread *self = rt_thread_in(&stack_dummy);
	*self = (struct rt_thread){ .magic = MAGIC, .tid = L4_Myself(), .alive = true };
	int n = tss_create(&epoch_tss, &epoch_first_dtor);
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
