
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <ccan/likely/likely.h>
#include <ccan/darray/darray.h>
#include <ccan/htable/htable.h>
#include <ccan/list/list.h>

#include <sneks/mm.h>
#include <sneks/hash.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include "sysmem-defs.h"


#define STKSIZE (PAGE_SIZE * 2)


struct thrd
{
	L4_ThreadId_t tid;	/* global */
	void *stkbase;

	/* exit/join syncing. */
	_Atomic int res;
	_Atomic bool alive;
	struct thrd *_Atomic joiner;
};


typedef darray(void *) tssdata;


static size_t hash_thrd(const void *key, void *priv);


static atomic_flag tss_meta_lock = ATOMIC_FLAG_INIT;
static darray(tss_dtor_t) tss_meta = darray_new();
static struct htable thrd_hash = HTABLE_INITIALIZER(
	thrd_hash, &hash_thrd, NULL);


static void thrd_init(void)
{
	/* track the first thread. */
	struct thrd *t = malloc(sizeof *t);
	*t = (struct thrd){ .tid = L4_Myself(), .alive = true };
	htable_add(&thrd_hash, hash_thrd(t, NULL), t);
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

static size_t hash_thrd(const void *key, void *priv) {
	const struct thrd *t = key;
	return int_hash(L4_ThreadNo(t->tid));
}


static bool cmp_thrd_t(const void *cand, void *key) {
	thrd_t *k = key;
	const struct thrd *c = cand;
	return L4_ThreadNo(c->tid) == *k;
}


thrd_t thrd_current(void) {
	return L4_ThreadNo(L4_Myself());
}


static void wrapper_fn(thrd_start_t fn, void *ptr) {
	thrd_exit((*fn)(ptr));
}


int thrd_create(thrd_t *thread, thrd_start_t fn, void *param_ptr)
{
	struct thrd *t = malloc(sizeof *t);
	if(t == NULL) return thrd_nomem;
	t->stkbase = aligned_alloc(STKSIZE, STKSIZE);
	if(t->stkbase == NULL) { free(t); return thrd_nomem; }
	/* for now, use sequential IDs starting from the first thread to call
	 * thrd_create(), i.e. main().
	 */
	static int next_tno = -1;
	if(unlikely(next_tno < 0)) {
		thrd_init();
		next_tno = thrd_current() + 1;
	}
	t->tid = L4_GlobalId(next_tno++, 2);
	t->alive = true;
	t->res = -1;
	t->joiner = NULL;
	int n = __sysmem_add_thread(L4_Pager(), L4_Myself().raw, t->tid.raw);
	if(n != 0) {
		printf("%s: add_thread failed, n=%d\n", __func__, n);
		goto err;
	}

	bool ok = htable_add(&thrd_hash, hash_thrd(t, NULL), t);
	if(!ok) goto err;

	uintptr_t top = ((uintptr_t)t->stkbase + STKSIZE - 16) & ~0xfu;
#ifdef __SSE__
	top += 4;
#endif
	L4_Word_t *p = (L4_Word_t *)top;
	*(--p) = (L4_Word_t)param_ptr;
	*(--p) = (L4_Word_t)fn;
	*(--p) = 0xfeedf007;	/* why else would it be in your mouth? */
	uint16_t ec = 0;
	n = __sysmem_breath_of_life(L4_Pager(), &ec, t->tid.raw,
		(L4_Word_t)&wrapper_fn, (L4_Word_t)p);
	if(n != 0 || ec != 0) {
		printf("%s: breath of life failed, n=%d, ec=%u\n", __func__, n, ec);
		__sysmem_rm_thread(L4_Pager(), L4_Myself().raw, t->tid.raw);
		goto err;
	}

	*thread = L4_ThreadNo(t->tid);
	return thrd_success;

err:
	free(t->stkbase);
	free(t);
	return thrd_error;
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

	/* TODO: protect thrd_hash with a mutex */
	thrd_t self = thrd_current();
	struct thrd *t = htable_get(&thrd_hash, int_hash(self),
		&cmp_thrd_t, &self);
	/* set up for passive exit. */
	atomic_store(&t->res, res);
	atomic_store(&t->alive, false);
	if(t->joiner != NULL) {
		/* active exit happened instead. */
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
		L4_LoadMR(1, res);
		L4_Send(t->joiner->tid);
		htable_del(&thrd_hash, int_hash(self), t);
		free(t->stkbase);
		free(t);
		__sysmem_rm_thread(L4_Pager(), L4_Myself().raw, L4_Myself().raw);
	} else {
		/* (other side disposes @t.) */
	}

	/* this halts the thread. (but loop around it just in case.) */
	for(;;) {
		asm volatile ("int $0xd0");	/* die die die */
		L4_Sleep(L4_Never);
	}
}


int thrd_join(thrd_t thr, int *res_p)
{
	/* TODO: protect thrd_hash with a mutex */
	struct thrd *t = htable_get(&thrd_hash, int_hash(thr), &cmp_thrd_t, &thr);
	if(t == NULL || t->joiner != NULL) return thrd_error;
	L4_ThreadId_t tid = t->tid;
again:
	if(!atomic_load(&t->alive)) {
		/* active join. */
		htable_del(&thrd_hash, int_hash(thr), t);
		/* (unlock here.) */
		if(res_p != NULL) *res_p = t->res;
		free(t->stkbase);
		free(t);
		int n = __sysmem_rm_thread(L4_Pager(), L4_Myself().raw, tid.raw);
		return n == 0 ? thrd_success : thrd_error;
	} else {
		/* passive join. */
		thrd_t self = thrd_current();
		t->joiner = htable_get(&thrd_hash, int_hash(self),
			&cmp_thrd_t, &self);
		assert(t->joiner != NULL);
		if(!atomic_load(&t->alive)) goto again;
		/* (unlock here.) */
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Receive(tid);
		if(L4_IpcFailed(tag)) return thrd_error;
		else {
			L4_Word_t resw; L4_StoreMR(1, &resw);
			if(res_p != NULL) *res_p = resw;
			return thrd_success;
		}
	}
}
