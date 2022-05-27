/* C11 condition variables. */
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <ccan/likely/likely.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/syscall.h>
#include <sneks/thrd.h>

#define EX (~0ul) /* norwegian blue? that an airline or what? */
#define WAKEUP 0x634f /* "cO" */

int cnd_init(cnd_t *cond) {
	atomic_store(cond, 0);
	return thrd_success;
}

void cnd_destroy(cnd_t *cond) {
	cnd_broadcast(&(cnd_t){ atomic_exchange(cond, EX) });
}

static struct thrd_wait *dequeue(cnd_t *cond)
{
	struct thrd_wait *w;
	L4_Word_t old = atomic_load(cond);
	do {
		if(unlikely(old == EX)) return NULL;
		w = (struct thrd_wait *)old;
	} while(w != NULL && !atomic_compare_exchange_strong(cond, &old, w->cnd.next));
	return w;
}

static int poke(struct thrd_wait *w)
{
	if(w == NULL) return thrd_success;
	L4_MsgTag_t tag;
	do {
		L4_LoadMR(0, (L4_MsgTag_t){ .X.label = WAKEUP }.raw);
		tag = L4_Send(w->tid);
	} while(L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3);
	return L4_IpcSucceeded(tag) ? thrd_success : thrd_error;
}

int cnd_signal(cnd_t *cond) {
	return poke(dequeue(cond));
}

int cnd_broadcast(cnd_t *cond) {
	struct thrd_wait *w;
	int status = thrd_success;
	do status |= poke(w = dequeue(cond)); while(w != NULL);
	return status;
}

int cnd_wait(cnd_t *cond, mtx_t *mutex)
{
	struct thrd_wait *w = __thrd_get_wait();
	w->cnd.next = atomic_load(cond);
	while(likely(w->cnd.next != EX) && !atomic_compare_exchange_strong(cond, &w->cnd.next, (L4_Word_t)w)) /* rave, repeat */ ;
	if(w->cnd.next == EX) { __thrd_put_wait(w); return thrd_error; }
	mtx_unlock(mutex);
	L4_MsgTag_t tag;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_WaitLocal_Timeout(L4_Never, &(L4_ThreadId_t){ });
	} while((L4_IpcFailed(tag) && L4_ErrorCode() >> 1 == 3) || (L4_IpcSucceeded(tag) && L4_Label(tag) != WAKEUP));
	assert(L4_IpcSucceeded(tag)); /* timeout is ∞, partner is wildcard, looped on cancel */
	__thrd_put_wait(w);
	mtx_lock(mutex);
	return thrd_success;
}

int cnd_timedwait(cnd_t *cond, mtx_t *mutex, const struct timespec *timeo) {
	/* TODO */
	return thrd_error;
}
