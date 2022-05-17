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
#include <sneks/thrd.h>

/* flags in state bits 5..0 */
#define LOCKED 1
#define CONFLICT 2

#define MAGIC 0xaf06cab5

struct __mtx_gubbins {
	size_t magic;
	_Atomic L4_Word_t s;
	struct list_head waits;
	size_t __pad[4];
};

static noreturn int ser_fn(void *);

static once_flag init_flag = ONCE_FLAG_INIT;
static L4_ThreadId_t ser_ltid = { .raw = 0 };

static void __mtx_init(void) {
	thrd_t ser_thrd;
	if(thrd_create(&ser_thrd, &ser_fn, NULL) != thrd_success || thrd_detach(ser_thrd) != thrd_success) abort();
	ser_ltid = L4_LocalIdOf(tidof(ser_thrd));
}

static bool ser_lock_op(L4_ThreadId_t *sender_p, struct __mtx_gubbins *m, struct thrd_wait *w)
{
	L4_Word_t prev = atomic_load(&m->s), next = sender_p->raw | LOCKED;
	if(prev == 0 && atomic_compare_exchange_strong(&m->s, &prev, next)) return true;
	if(~prev & CONFLICT) {
		next = prev | CONFLICT;
		if(!atomic_compare_exchange_weak(&m->s, &prev, next)) return ser_lock_op(sender_p, m, w);
	}
	assert(w->tid.raw == sender_p->raw);
	list_add_tail(&m->waits, &w->mtx.link);
	return false;
}

static bool ser_unlock_op(L4_MsgTag_t tag, L4_ThreadId_t *sender_p, struct __mtx_gubbins *m)
{
	L4_Word_t prev = atomic_load(&m->s), next;
	if(unlikely((prev & ~0x3f) != sender_p->raw)) {
		L4_ThreadId_t g = L4_GlobalIdOf(*sender_p);
		fprintf(stderr, "bad unlock; ltid=%#lx (%lu:%lu), mtx=%#lx\n", sender_p->raw, L4_ThreadNo(g), L4_Version(g), prev);
		return false; /* TODO: bump curse counter */
	}
	if(list_empty(&m->waits)) next = 0;
	else {
		struct thrd_wait *w = list_pop(&m->waits, struct thrd_wait, mtx.link);
		assert(L4_IsLocalId(w->tid) && !L4_IsNilThread(w->tid));
		next = w->tid.raw | CONFLICT | LOCKED;
		L4_ThreadId_t later = w->tid;
		if(!list_empty(&m->waits) || L4_IpcXcpu(tag)) { later = *sender_p; *sender_p = w->tid; } /* pref to schedule sleeper so conflict drains faster */
		L4_LoadMR(0, 0); L4_Reply(later);
	}
	L4_Word_t ex = atomic_exchange(&m->s, next);
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
			struct __mtx_gubbins *m = (void *)mutex_ptr; assert(m->magic == MAGIC);
			bool reply;
			switch(L4_Label(tag)) {
				case 'L': reply = ser_lock_op(&sender, m, (struct thrd_wait *)wait_ptr); break;
				case 'U': reply = ser_unlock_op(tag, &sender, m); break;
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

int mtx_init(mtx_t *mptr, int type)
{
	if(type != mtx_plain) return thrd_error;
	static_assert(sizeof(struct __mtx_gubbins) == sizeof(mtx_t));
	struct __mtx_gubbins *m = (void *)mptr;
	*m = (struct __mtx_gubbins){ .magic = MAGIC };
	list_head_init(&m->waits);
	atomic_thread_fence(memory_order_release);
	return thrd_success;
}

void mtx_destroy(mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	if(m->magic != MAGIC || !list_empty(&m->waits)) {
		/* illegal states, but let them slide anyway.
		 * TODO: bump a curse counter so this doesn't reoccur unbounded.
		 */
		fprintf(stderr, "%s: illegal state on mtx=%p (ignored)\n", __func__, mptr);
	}
	L4_Word_t s = atomic_exchange(&m->s, ~0ul);
	assert(s == 0 || s == (L4_MyLocalId().raw | 1));
	m->magic = ~MAGIC;
}

int mtx_lock(mtx_t *mptr)
{
	struct __mtx_gubbins *m = (void *)mptr;
	if(unlikely(m->magic != MAGIC)) return thrd_error;
	L4_Word_t prev = atomic_load(&m->s), next = L4_MyLocalId().raw | LOCKED;
	if(likely(prev == 0 && atomic_compare_exchange_strong(&m->s, &prev, next))) return thrd_success;
	if(~prev & CONFLICT) {
		next = prev | CONFLICT;
		if(!atomic_compare_exchange_weak(&m->s, &prev, next)) return mtx_lock(mptr);
	}
	if(unlikely(ser_ltid.raw == 0)) call_once(&init_flag, &__mtx_init);
	struct thrd_wait *w = __thrd_get_wait();
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.label = 'L' }.raw); L4_LoadMR(1, (L4_Word_t)m); L4_LoadMR(2, (L4_Word_t)w);
	L4_MsgTag_t tag = L4_Lcall(ser_ltid);
	__thrd_put_wait(w);
	if(L4_IpcFailed(tag)) {
		fprintf(stderr, "%s: ipc-serialized lock failed, ec=%#lx\n", __func__, L4_ErrorCode());
		return mtx_lock(mptr);
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
	if(unlikely(m->magic != MAGIC)) return thrd_error;
	L4_Word_t prev = L4_MyLocalId().raw | LOCKED;
	if(likely(atomic_compare_exchange_strong(&m->s, &prev, 0))) return thrd_success;
	assert(prev == (L4_MyLocalId().raw | LOCKED | CONFLICT));
	if(unlikely(ser_ltid.raw == 0)) call_once(&init_flag, &__mtx_init);
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 'U' }.raw); L4_LoadMR(1, (L4_Word_t)mptr);
	if(L4_IpcFailed(L4_Lcall(ser_ltid))) { /* TODO: add curse */
		fprintf(stderr, "%s: ipc-serialized unlock failed, ec=%#lx\n", __func__, L4_ErrorCode());
	}
	return thrd_success;
}
