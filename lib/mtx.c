/* C11 mutexes, except for timed and recursive modes, and timeout-waiting. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <threads.h>
#include <assert.h>
#include <errno.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <ccan/likely/likely.h>
#include <ccan/list/list.h>

/* flags (the low six bits) */
#define LOCKED 1
#define CONFLICT 2

#define MAGIC 0xaf06cab5

struct __mtx_gubbins {
	size_t magic;
	_Atomic L4_Word_t s;
	struct list_head waits;
	size_t __pad[4];
};

struct wait {
	struct list_node link; /* in __mtx_gubbins.waits */
	mtx_t *mutex;
	L4_ThreadId_t ltid;
};

static thrd_t ser_thrd;
static tss_t w_key;

/* what the serializer doin */
static bool ser_lock_op(L4_ThreadId_t *sender_p, mtx_t *mptr, struct wait *w)
{
	struct __mtx_gubbins *m = (void *)mptr;
	assert(m->magic == MAGIC);
	L4_Word_t prev = atomic_load(&m->s), next = sender_p->raw | LOCKED;
	if(prev == 0 && atomic_compare_exchange_strong(&m->s, &prev, next)) return true;
	if(~prev & CONFLICT) {
		next = prev | CONFLICT;
		if(!atomic_compare_exchange_weak(&m->s, &prev, next)) return ser_lock_op(sender_p, mptr, w);
	}
	assert(w->mutex == mptr && L4_SameThreads(w->ltid, *sender_p));
	list_add_tail(&m->waits, &w->link);
	return false;
}

static bool ser_unlock_op(L4_ThreadId_t *sender_p, mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	assert(m->magic == MAGIC);
	L4_Word_t prev = atomic_load(&m->s), next;
	if(unlikely((prev & ~0x3f) != sender_p->raw)) {
		L4_ThreadId_t g = L4_GlobalIdOf(*sender_p);
		fprintf(stderr, "bad unlock; ltid=%#lx (%lu:%lu), mtx=%#lx\n", sender_p->raw, L4_ThreadNo(g), L4_Version(g), prev);
		return false; /* TODO: bump curse counter */
	}
	if(list_empty(&m->waits)) next = 0;
	else {
		struct wait *w = list_pop(&m->waits, struct wait, link);
		next = w->ltid.raw | CONFLICT | LOCKED;
		L4_LoadMR(0, 0); L4_Reply(*sender_p); /* wake caller out of line */
		*sender_p = w->ltid; /* Lipc back into waiter */
	}
	L4_Word_t ex = atomic_exchange_explicit(&m->s, next, memory_order_acq_rel);
	if(ex != prev) fprintf(stderr, "%s: mutex was unlocked from under our feet??\n", __func__); /* TODO: add curse */
	return true;
}

static noreturn int ser_fn(void *param_ptr)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_Never, &sender);
		for(;;) {
			if(L4_IpcFailed(tag)) { fprintf(stderr, "%s: ipc failed, ec=%lu\n", __func__, L4_ErrorCode()); break; }
			L4_Word_t mutex_ptr, wait_ptr;
			L4_StoreMR(1, &mutex_ptr); L4_StoreMR(2, &wait_ptr);
			bool reply;
			switch(L4_Label(tag)) {
				case 'L': reply = ser_lock_op(&sender, (mtx_t *)mutex_ptr, (struct wait *)wait_ptr); break;
				case 'U': reply = ser_unlock_op(&sender, (mtx_t *)mutex_ptr); break;
				default: fprintf(stderr, "%s: unhandled tag=%#lx\n", __func__, tag.raw); reply = false;
			}
			if(!reply) break;
			L4_Accept(L4_UntypedWordsAcceptor);
			L4_LoadMR(0, 0);
			/* LreplyWaitLocal() */
			tag = L4_Lipc(sender, L4_anylocalthread, L4_Timeouts(L4_ZeroTime, L4_Never), &sender);
		}
	}
}

static void __mtx_init(void) {
	static_assert(sizeof(struct __mtx_gubbins) == sizeof(mtx_t));
	if(tss_create(&w_key, &free) != thrd_success) abort();
	if(thrd_create(&ser_thrd, &ser_fn, NULL) != thrd_success) abort();
	if(thrd_detach(ser_thrd) != thrd_success) abort();
}

int mtx_init(mtx_t *mptr, int type)
{
	if(type != mtx_plain) return thrd_error;
	static once_flag of = ONCE_FLAG_INIT; call_once(&of, &__mtx_init);
	struct __mtx_gubbins *m = (void *)mptr;
	*m = (struct __mtx_gubbins){ .magic = MAGIC };
	list_head_init(&m->waits);
	atomic_thread_fence(memory_order_release);
	return thrd_success;
}

void mtx_destroy(mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	if(m->magic != MAGIC || (m->s != 0 && m->s != (L4_MyLocalId().raw | 1)) || !list_empty(&m->waits)) {
		/* illegal states, but let them slide anyway.
		 * TODO: bump a curse counter so this doesn't reoccur unbounded.
		 */
		fprintf(stderr, "%s: illegal state on mtx=%p (ignored)\n", __func__, mptr);
	}
	atomic_store_explicit(&m->s, ~0ul, memory_order_relaxed);
	atomic_store_explicit(&m->magic, ~MAGIC, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
}

int mtx_lock(mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	L4_Word_t prev = atomic_load_explicit(&m->s, memory_order_relaxed), next = L4_MyLocalId().raw | LOCKED;
	if(prev == 0 && atomic_compare_exchange_strong_explicit(&m->s, &prev, next, memory_order_acquire, memory_order_relaxed)) return thrd_success;
	if(unlikely(L4_IsNilThread(tidof(ser_thrd)))) {
		/* early mtx_lock such as during dlmalloc init. */
		L4_Sleep(L4_TimePeriod(2000)); /* way too long; make it two scheduling precisions per KIP */
		return mtx_lock(mptr);
	}
	if(~prev & CONFLICT) {
		next = prev | CONFLICT;
		if(!atomic_compare_exchange_weak(&m->s, &prev, next)) return mtx_lock(mptr);
	}
	struct wait *w = tss_get(w_key);
	if(w == NULL) {
		if(w = malloc(sizeof *w), w == NULL) return thrd_error;
		tss_set(w_key, w);
	}
	*w = (struct wait){ .mutex = mptr, .ltid = L4_MyLocalId() };
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.label = 'L' }.raw); L4_LoadMR(1, (L4_Word_t)mptr); L4_LoadMR(2, (L4_Word_t)w);
	if(L4_IpcFailed(L4_Lcall(tidof(ser_thrd)))) {
		fprintf(stderr, "%s: ipc-serialized lock failed, ec=%#lx\n", __func__, L4_ErrorCode());
		return thrd_error;
	}
	assert((m->s & ~0x3ful) == L4_MyLocalId().raw);
	return thrd_success;
}

int mtx_trylock(mtx_t *mptr) {
	L4_Word_t prev = 0, next = L4_MyLocalId().raw | LOCKED;
	struct __mtx_gubbins *m = (void *)mptr;
	return atomic_compare_exchange_strong(&m->s, &prev, next) ? thrd_success : thrd_busy;
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
	/* invalid type, since mtx_init() never allows timed mutexes. */
	return thrd_error;
}

int mtx_unlock(mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	L4_Word_t prev = L4_MyLocalId().raw | LOCKED;
	if(atomic_compare_exchange_strong(&m->s, &prev, 0)) return thrd_success;
	assert(prev == (L4_MyLocalId().raw | LOCKED | CONFLICT));
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 'U' }.raw); L4_LoadMR(1, (L4_Word_t)mptr);
	if(L4_IpcFailed(L4_Lcall(tidof(ser_thrd)))) {
		fprintf(stderr, "%s: ipc-serialized unlock failed, ec=%#lx\n", __func__, L4_ErrorCode());
		return thrd_error;
	}
	return thrd_success;
}
