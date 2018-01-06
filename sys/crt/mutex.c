
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <errno.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <ccan/likely/likely.h>
#include <ccan/compiler/compiler.h>
#include <ccan/list/list.h>

#include <sneks/rbtree.h>
#include <sneks/thread.h>


/* special mutex states */
#define MTX_UNOWNED ((L4_Word_t)0)
#define MTX_DEAD (~MTX_UNOWNED)
/* flags (the low six bits) */
#define MTX_LOCKED 1
#define MTX_CONFLICT 2

/* operations of the serializer thread */
#define MUTEX_LOCK_LABEL 0x4321
#define MUTEX_UNLOCK_LABEL 0x4322


/* waitlists and such, per mutex. */
struct mtx_info
{
	struct rb_node rb;	/* in info_tree */
	mtx_t *mutex;
	struct list_head waitlist;	/* struct mtx_wait via wait_link */
};


static thrd_t ser_thrd;
static L4_ThreadId_t mutex_serializer_tid = { .raw = 0 };

/* private state of ser_fn(), initialized in init_mutexes() */
static struct rb_root info_tree;


/* accessors for info_tree. */

static inline struct mtx_info *insert_mtx_info_helper(
	struct rb_root *root, struct mtx_info *mi)
{
	struct rb_node **p = &root->rb_node, *parent = NULL;
	while(*p != NULL) {
		parent = *p;
		struct mtx_info *oth = rb_entry(parent, struct mtx_info, rb);
		intptr_t v = (intptr_t)mi->mutex - (intptr_t)oth->mutex;
		if(v < 0) p = &(*p)->rb_left;
		else if(v > 0) p = &(*p)->rb_right;
		else return oth;
	}
	rb_link_node(&mi->rb, parent, p);
	return NULL;
}


static void insert_mtx_info(struct mtx_info *mi)
{
	struct mtx_info *dupe = insert_mtx_info_helper(&info_tree, mi);
	if(dupe != NULL) {
		fprintf(stderr, "mtx_info for mutex=%p already in tree??\n",
			mi->mutex);
		abort();
	}
	__rb_insert_color(&mi->rb, &info_tree);
}


static struct mtx_info *find_mtx_info(mtx_t *mutex)
{
	struct rb_node *n = info_tree.rb_node;
	struct mtx_info *mi;
	while(n != NULL) {
		mi = rb_entry(n, struct mtx_info, rb);
		intptr_t v = (intptr_t)mutex - (intptr_t)mi->mutex;
		if(v < 0) n = n->rb_left;
		else if(v > 0) n = n->rb_right;
		else {
			assert(mi->mutex == mutex);
			return mi;
		}
	}
	return NULL;
}


/* serialized access operations. */

static bool ser_lock_op(L4_ThreadId_t *sender_p, mtx_t *mutex)
{
	assert(L4_IsLocalId(*sender_p));
	L4_Word_t prev, next;

retry:
	/* to be as valid as the correct sequence of atomic operations in the
	 * client thread, we must do that same sequence here.
	 */
	prev = atomic_load(mutex);
	next = sender_p->raw | MTX_LOCKED;
	if(prev == MTX_UNOWNED
		&& atomic_compare_exchange_strong(mutex, &prev, next))
	{
		/* simple success. exit immediately. */
		return true;
	}

	if(!(prev & MTX_CONFLICT)) {
		/* set conflict bit. retry if mutex was released in between. */
		next = prev | MTX_CONFLICT;
		if(!atomic_compare_exchange_weak(mutex, &prev, next)) goto retry;
	}

	/* add wait item and go back to IPC sleep. */
	struct mtx_info *info = find_mtx_info(mutex);
	assert(info != NULL);

	struct mtx_wait *w = &thrd_from_tid(*sender_p)->mw;
	w->ltid = *sender_p;
	list_add_tail(&info->waitlist, &w->wait_link);

	return false;
}


static bool ser_unlock_op(L4_ThreadId_t *sender_p, mtx_t *mutex)
{
	assert(L4_IsLocalId(*sender_p));

	L4_Word_t prev = atomic_load(mutex), next;
	if(unlikely((prev & ~0x3f) != sender_p->raw)) {
		L4_ThreadId_t g = L4_GlobalIdOf(*sender_p);
		fprintf(stderr, "%s: bad unlock; ltid=%#lx (%lu:%lu), mtx=%#lx\n",
			__func__, sender_p->raw, L4_ThreadNo(g), L4_Version(g), prev);
		/* fuck 'em for ending up that way. */
		return false;
	}

	struct mtx_info *info = find_mtx_info(mutex);
	assert(info != NULL);
	if(list_empty(&info->waitlist)) {
		/* no more waiters; unlock it all the way. */
		next = MTX_UNOWNED;
	} else {
		/* switch to the next waiter.
		 *
		 * NOTE: could clear the conflict bit also, so that the serializer can
		 * bow out one unlock sooner, but that's an optimization which doesn't
		 * belong in the testbench runtime.
		 */
		struct mtx_wait *w = list_pop(&info->waitlist,
			struct mtx_wait, wait_link);
		next = w->ltid.raw | MTX_CONFLICT | MTX_LOCKED;

		/* wake the unlocker up. */
		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Reply(*sender_p);
		if(L4_IpcFailed(tag)) {
			fprintf(stderr, "%s: couldn't wake caller, ec=%#lx\n",
				__func__, L4_ErrorCode());
		}

		/* do a little switcheroo just to be pretty about it. */
		*sender_p = w->ltid;
	}

	if(unlikely(!atomic_compare_exchange_strong(mutex, &prev, next))) {
		/* this, too, is an error. */
		fprintf(stderr, "%s: mutex was unlocked from under our feet??\n",
			__func__);
		abort();	/* retry would be invalid because of altered *sender_p */
	}

	return true;
}


/* mutex access serialization thread. */
static int ser_fn(void *param_ptr UNUSED)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_Never, &sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				fprintf(stderr, "%s: ipc failed, ec=%#lx\n",
					__func__, L4_ErrorCode());
				break;
			}

			bool reply;
			switch(L4_Label(tag)) {
				case MUTEX_LOCK_LABEL: {
					L4_Word_t mutex_ptr; L4_StoreMR(1, &mutex_ptr);
					reply = ser_lock_op(&sender, (mtx_t *)mutex_ptr);
					break;
				}
				case MUTEX_UNLOCK_LABEL: {
					L4_Word_t mutex_ptr; L4_StoreMR(1, &mutex_ptr);
					reply = ser_unlock_op(&sender, (mtx_t *)mutex_ptr);
					break;
				}
				default:
					fprintf(stderr, "%s: unhandled tag=%#lx\n",
						__func__, tag.raw);
					reply = false;
			}
			if(!reply) break;
			else {
				/* all replies are empty, control transfer only. */
				L4_Accept(L4_UntypedWordsAcceptor);
				L4_LoadMR(0, 0);
				/* LreplyWaitLocal() */
				tag = L4_Lipc(sender, L4_anylocalthread,
					L4_Timeouts(L4_ZeroTime, L4_Never), &sender);
			}
		}
	}
	return 0;
}


static COLD void actual_init_mutexes(void)
{
	info_tree = RB_ROOT;

	int n = thrd_create(&ser_thrd, &ser_fn, NULL);
	if(n != thrd_success) {
		fprintf(stderr, "%s: can't create serializer thread\n", __func__);
		abort();
	}
	mutex_serializer_tid = thrd_to_tid(ser_thrd);
	assert(!L4_IsNilThread(mutex_serializer_tid));
}


/* public interface. */

int mtx_init(mtx_t *mtx, int type)
{
	static once_flag init_once = ONCE_FLAG_INIT;
	call_once(&init_once, &actual_init_mutexes);

	if(type != mtx_plain) return thrd_error;

	/* FIXME: serialize access to the mtx_info tree */
	struct mtx_info *info = find_mtx_info(mtx);
	if(info != NULL) return thrd_error;	/* reinitialization. */
	info = aligned_alloc(64, sizeof *info);
	if(info == NULL) return thrd_error;
	info->mutex = mtx;
	list_head_init(&info->waitlist);
	insert_mtx_info(info);

	*mtx = MTX_UNOWNED;
	return thrd_success;
}


void mtx_destroy(mtx_t *mtx)
{
	/* mutexes that aren't unowned, or owned by the local thread and not
	 * conflicted on, mustn't be destroyed.
	 */
	if(*mtx != MTX_UNOWNED && *mtx != (L4_MyLocalId().raw | 1)) {
		fprintf(stderr, "attempted to destroy busy mutex at %p\n", mtx);
		abort();
	}

	*mtx = MTX_DEAD;

	struct mtx_info *info = find_mtx_info(mtx);
	if(info == NULL) {
		/* not initialized. let this one go.
		 *
		 * TODO: bump a curse counter as well so that this doesn't reoccur
		 * unbounded and unnoticed.
		 */
		return;
	}

	/* FIXME: serialize access to the mtx_info tree */
	__rb_erase(&info->rb, &info_tree);
	assert(list_empty(&info->waitlist));
	free(info);
}


int mtx_lock(mtx_t *mtx)
{
	L4_Word_t prev = atomic_load(mtx), next;

retry:
	next = L4_MyLocalId().raw | MTX_LOCKED;
	if(prev == MTX_UNOWNED
		&& atomic_compare_exchange_strong(mtx, &prev, next))
	{
		/* fastpath exit. */
		return thrd_success;
	}

	if(unlikely(L4_IsNilThread(mutex_serializer_tid))) {
		/* soft fallback for when the serializer isn't active (yet). happens
		 * because of dlmalloc's mutexes.
		 *
		 * FIXME: this is likely unnecessary in sneks syscrt. see if that's
		 * true & remove if so.
		 */
		L4_Sleep(L4_TimePeriod(2000));	/* way too long */
		prev = atomic_load(mtx);
		goto retry;
	}

	/* mark conflict where necessary, and go to wait. */
	assert(prev & MTX_LOCKED);
	if(!(prev & MTX_CONFLICT)) {
		next = prev | MTX_CONFLICT;
		if(!atomic_compare_exchange_weak(mtx, &prev, next)) {
			/* failed; try again. */
			goto retry;
		}
	}
#ifdef DEBUG_ME_HARDER
	printf("%s: calling serializer from tid=%lu:%lu (return address %p)\n",
		__func__, L4_ThreadNo(L4_Myself()), L4_Version(L4_Myself()),
		__builtin_return_address(0));
#endif
	L4_MsgTag_t tag = (L4_MsgTag_t){ .X.u = 1, .X.label = MUTEX_LOCK_LABEL };
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, tag.raw);
	L4_LoadMR(1, (L4_Word_t)mtx);
	tag = L4_Lcall(mutex_serializer_tid);
	if(L4_IpcFailed(tag)) {
		fprintf(stderr, "%s: ipc-serialized lock failed, ec=%#lx\n",
			__func__, L4_ErrorCode());
		return thrd_error;
	}

	assert((atomic_load(mtx) & ~0x3ful) == L4_MyLocalId().raw);
	return thrd_success;
}


int mtx_trylock(mtx_t *mtx)
{
	L4_Word_t prev = MTX_UNOWNED, next = L4_MyLocalId().raw | 1;
	return atomic_compare_exchange_strong(mtx, &prev, next)
		? thrd_success : thrd_busy;
}


int mtx_timedlock(mtx_t *mtx, const struct timespec *ts)
{
	/* invalid type, since mtx_init() never allows timed mutexes. */
	return thrd_error;
}


int mtx_unlock(mtx_t *mtx)
{
	L4_Word_t prev = L4_MyLocalId().raw | MTX_LOCKED, next = MTX_UNOWNED;
	if(atomic_compare_exchange_strong(mtx, &prev, next)) {
		/* fastpath */
		return thrd_success;
	}
	assert(prev & MTX_CONFLICT);

#ifdef DEBUG_ME_HARDER
	printf("%s: calling serializer from tid=%lu:%lu (return address %p)\n",
		__func__, L4_ThreadNo(L4_Myself()), L4_Version(L4_Myself()),
		__builtin_return_address(0));
#endif
	L4_MsgTag_t tag = (L4_MsgTag_t){ .X.u = 1,
		.X.label = MUTEX_UNLOCK_LABEL };
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, tag.raw);
	L4_LoadMR(1, (L4_Word_t)mtx);
	tag = L4_Lcall(mutex_serializer_tid);
	if(L4_IpcFailed(tag)) {
		fprintf(stderr, "%s: ipc-serialized unlock failed, ec=%#lx\n",
			__func__, L4_ErrorCode());
		return thrd_error;
	}

	return thrd_success;
}
