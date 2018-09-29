
/* C11 condition variables. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/syscall.h>


struct cnd_waiter {
	L4_Word_t next;
	L4_ThreadId_t tid;
};


static once_flag init_flag = ONCE_FLAG_INIT;
static tss_t waiter_key;
static bool init_done = false;


static void cnd_tss_init(void)
{
	int n = tss_create(&waiter_key, &free);
	if(n != thrd_success) {
		fprintf(stderr, "%s: tss_create failed, n=%d\n", __func__, n);
		abort();
	}
	init_done = true;
}


int cnd_init(cnd_t *cond)
{
	atomic_store_explicit(cond, 0, memory_order_relaxed);
	return thrd_success;
}


void cnd_destroy(cnd_t *cond)
{
	/* this setup is so lightweight that we can get correct results in the "no
	 * waiters" case without even doing anything, and undefined behaviour
	 * otherwise. just like it says on teh specz0r. so let's do just that. (or
	 * not?)
	 */
}


static struct cnd_waiter *take_waiter(cnd_t *cond)
{
	struct cnd_waiter *w;
	L4_Word_t old = atomic_load_explicit(cond, memory_order_relaxed);
	do {
		w = (struct cnd_waiter *)old;
		if(w == NULL) break;
	} while(!atomic_compare_exchange_strong_explicit(cond, &old, w->next,
		memory_order_acquire, memory_order_relaxed));

	return w;
}


static int poke_waiter(struct cnd_waiter *w)
{
	L4_MsgTag_t tag;
	do {
		L4_LoadMR(0, 0);
		tag = L4_Send(w->tid);
	} while(L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3);
	return L4_IpcSucceeded(tag) ? thrd_success : thrd_error;
}


int cnd_signal(cnd_t *cond) {
	struct cnd_waiter *w = take_waiter(cond);
	return w == NULL ? thrd_success : poke_waiter(w);
}


int cnd_broadcast(cnd_t *cond)
{
	for(;;) {
		struct cnd_waiter *w = take_waiter(cond);
		if(w == NULL) break;
		int n = poke_waiter(w);
		if(n != thrd_success) return n;
	}
	return thrd_success;
}


int cnd_wait(cnd_t *cond, mtx_t *mutex)
{
	if(!init_done) call_once(&init_flag, &cnd_tss_init);
	struct cnd_waiter *w = tss_get(waiter_key);
	if(w == NULL) {
		w = malloc(sizeof *w);
		w->tid = L4_MyLocalId();
		tss_set(waiter_key, w);
	}

	L4_Word_t old = atomic_load_explicit(cond, memory_order_relaxed);
	do {
		w->next = old;
	} while(!atomic_compare_exchange_weak_explicit(cond,
		&old, (L4_Word_t)w, memory_order_release, memory_order_relaxed));
	mtx_unlock(mutex);

	L4_ThreadId_t sender;
	L4_MsgTag_t tag;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_WaitLocal_Timeout(L4_Never, &sender);
	} while(L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3);
	mtx_lock(mutex);

	return L4_IpcSucceeded(tag) ? thrd_success : thrd_error;
}


int cnd_timedwait(cnd_t *cond, mtx_t *mutex, const struct timespec *timeo)
{
	return thrd_error;
}
