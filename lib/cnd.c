/* C11 condition variables. */
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/syscall.h>

struct cnd_waiter {
	L4_Word_t next;
	L4_ThreadId_t tid;
};

static tss_t waiter_key;

static void __cnd_init(void) {
	if(tss_create(&waiter_key, &free) != thrd_success) abort();
}

int cnd_init(cnd_t *cond) {
	static once_flag of = ONCE_FLAG_INIT;
	call_once(&of, &__cnd_init);
	atomic_store_explicit(cond, 0, memory_order_relaxed);
	return thrd_success;
}

void cnd_destroy(cnd_t *cond) {
	cnd_broadcast(cond);
	assert(*cond == 0);
	atomic_store_explicit(cond, ~0ul, memory_order_release);
}

static struct cnd_waiter *take_waiter(cnd_t *cond)
{
	struct cnd_waiter *w;
	L4_Word_t old = atomic_load_explicit(cond, memory_order_relaxed);
	if(old == ~0ul) return NULL;
	do {
		w = (struct cnd_waiter *)old;
	} while(w != NULL && !atomic_compare_exchange_strong_explicit(cond, &old, w->next, memory_order_acq_rel, memory_order_relaxed));
	return w;
}

static int poke_waiter(struct cnd_waiter *w)
{
	if(w == NULL) return thrd_success;
	L4_MsgTag_t tag;
	do {
		L4_LoadMR(0, 0);
		tag = L4_Send(w->tid);
	} while(L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3);
	return L4_IpcSucceeded(tag) ? thrd_success : thrd_error;
}

int cnd_signal(cnd_t *cond) {
	return poke_waiter(take_waiter(cond));
}

int cnd_broadcast(cnd_t *cond) {
	struct cnd_waiter *w;
	int status = thrd_success;
	do status |= poke_waiter(w = take_waiter(cond)); while(w != NULL);
	return status;
}

int cnd_wait(cnd_t *cond, mtx_t *mutex)
{
	struct cnd_waiter *w = tss_get(waiter_key);
	if(w == NULL) {
		if(w = malloc(sizeof *w), w == NULL) return thrd_error;
		w->tid = L4_MyLocalId();
		tss_set(waiter_key, w);
	}
	w->next = atomic_load_explicit(cond, memory_order_relaxed);
	if(w->next == ~0ul) return thrd_error;
	while(!atomic_compare_exchange_strong_explicit(cond, &w->next, (L4_Word_t)w, memory_order_release, memory_order_relaxed)) /* revise until done */ ;
	mtx_unlock(mutex);
	L4_ThreadId_t sender;
	L4_MsgTag_t tag;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_WaitLocal_Timeout(L4_Never, &sender);
	} while(L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3);
	assert(L4_IpcSucceeded(tag)); /* timeout is ∞, partner is wildcard, looped on cancel */
	mtx_lock(mutex);
	return thrd_success;
}

int cnd_timedwait(cnd_t *cond, mtx_t *mutex, const struct timespec *timeo) {
	/* TODO */
	return thrd_error;
}
