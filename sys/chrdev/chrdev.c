
#define CHRDEVIMPL_IMPL_SOURCE
#undef BUILD_SELFTEST

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <ccan/htable/htable.h>
#include <ccan/darray/darray.h>
#include <ccan/minmax/minmax.h>
#include <ccan/compiler/compiler.h>

#include <sneks/process.h>
#include <sneks/thread.h>
#include <sneks/hash.h>
#include <sneks/msg.h>
#include <sneks/systask.h>
#include <sneks/rollback.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/proc-defs.h>
#include <sneks/chrdev.h>

#include "muidl.h"
#include "chrdev-impl-defs.h"
#include "private.h"


#define NUM_LC_EVENTS 128	/* at most 1024 */

/* handle flags. Sneks::IO::O_* starts at 2048, so far. */
#define HF_WRITE_BLOCKED 1
#define HF_NONBLOCK SNEKS_IO_O_NONBLOCK

#define CHR_IMPL(file) ((chrfile_t *)(file)->impl)
#define CHR_FILE(impl) ci2f((impl))

/* missing from CCAN darray: the order-destroying removal. */
#ifndef darray_remove_fast
#define darray_remove_fast(arr, i) do { \
		assert((arr).size > 0); \
		size_t index_ = (i); \
		assert(index_ < (arr).size); \
		assert(index_ >= 0); \
		if(index_ < --(arr).size) { \
			(arr).item[index_] = (arr).item[(arr).size]; \
		} \
	} while(false)
#endif


struct chrdev_client;
struct chrdev_handle;
struct chrdev_file;


struct chrdev_handle
{
	struct chrdev_file *file;
	struct chrdev_client *owner;
	uint16_t fd;
	uint16_t flags;	/* HF_* */
	int client_ix;	/* index in owner->handles */
	int file_ix;	/* index in file->handles */
	uint32_t events;/* set of EPOLLFOO for Poll::set_notify response */

	/* we track the first blocked read/write client in each handle; since
	 * userspace isn't multithreaded right now, there's no hash table where
	 * the others would go.
	 *
	 * HF_WRITE_BLOCKED says whether this is a reader or a writer.
	 */
	L4_ThreadId_t blocker;
};


struct chrdev_file
{
	darray(struct chrdev_handle *) handles;
	uint8_t impl[]	/* device implementor's chrfile_t */
		__attribute__((aligned(16)));
};


struct chrdev_client
{
	pid_t pid;
	L4_ThreadId_t notify_tid;
	int next_fd;
	darray(struct chrdev_handle *) handles;
};


/* lifecycle events. these are queued up in lifecycle_handler_fn() and
 * consumed in caller() and get_handle().
 */
struct lc_event
{
	unsigned short tag;	/* MPL_* */
	unsigned short primary;
	union {
		unsigned short child;
		struct {
			short signo;
			int exitcode, waitstatus;
		} exit;
	};
};


static void consume_lc_queue(void);
static void chrdev_confirm(L4_Word_t, void *);
static PURE_FUNCTION struct chrdev_client *caller(bool create);
static void client_dtor(struct chrdev_client *c);


static pid_t my_pid;
static L4_ThreadId_t main_tid;
static thrd_t poke_thrd;
static struct htable client_ht, handle_ht;
static size_t impl_size;

/* lifecycle handling */
static int lifecycle_msg = -1;	/* sysmsg handle */
/* an ad-hoc SPSC concurrent circular buffer. */
static struct lc_event lce_queue[NUM_LC_EVENTS];
/* its control word. 0..9 = read pos, 10..19 = write pos, 20 = overflow */
static _Atomic uint32_t lce_ctrl;


static size_t rehash_client(const void *ptr, void *priv) {
	const struct chrdev_client *c = ptr;
	return int_hash(c->pid);
}


static size_t rehash_handle(const void *ptr, void *priv) {
	const struct chrdev_handle *h = ptr;
	return int_hash(h->owner->pid << 16 | h->fd);
}


static bool cmp_client_to_pid(const void *cand, void *key) {
	const struct chrdev_client *c = cand;
	return c->pid == *(pid_t *)key;
}


static struct chrdev_file *ci2f(chrfile_t *chrfile) {
	uintptr_t p = (uintptr_t)chrfile;
	p -= offsetof(struct chrdev_file, impl);
	return (struct chrdev_file *)p;
}


/* remove handle from client and htable. */
static void remove_handle(struct chrdev_handle *h)
{
	struct chrdev_client *c = h->owner;
	assert(c->handles.item[h->client_ix] == h);
	darray_remove_fast(c->handles, h->client_ix);
	assert(h->client_ix == c->handles.size
		|| c->handles.item[h->client_ix]->client_ix != h->client_ix);
	c->handles.item[h->client_ix]->client_ix = h->client_ix;
#ifndef NDEBUG
	for(size_t i=0; i < c->handles.size; i++) {
		assert(c->handles.item[i]->client_ix == i);
	}
#endif

	bool ok = htable_del(&handle_ht, rehash_handle(h, NULL), h);
	assert(ok);
}


static int handle_dtor(struct chrdev_handle *h)
{
	struct chrdev_file *f = h->file;
	assert(f->handles.item[h->file_ix] == h);
	darray_remove_fast(f->handles, h->file_ix);
	assert(h->file_ix == f->handles.size
		|| f->handles.item[h->file_ix]->file_ix != h->file_ix);
	f->handles.item[h->file_ix]->file_ix = h->file_ix;
#ifndef NDEBUG
	for(size_t i=0; i < f->handles.size; i++) {
		assert(f->handles.item[i]->file_ix == i);
	}
#endif
	remove_handle(h);
	free(h);

	int n = 0;
	if(f->handles.size == 0) {
		n = (*callbacks.close)(CHR_IMPL(f));
		/* ... an error from the close callback won't stop the file from being
		 * destroyed; we'll just pass it upward.
		 */
#ifndef NDEBUG
		struct htable_iter it;
		for(h = htable_first(&handle_ht, &it);
			h != NULL; h = htable_next(&handle_ht, &it))
		{
			assert(h->file != f);
		}
#endif
		darray_free(f->handles);
		free(f);
	}

	return n;
}


static void client_dtor(struct chrdev_client *c)
{
	while(c->handles.size > 0) {
		handle_dtor(c->handles.item[0]);
	}
	bool ok = htable_del(&client_ht, int_hash(c->pid), c);
	assert(ok);
	darray_free(c->handles);
	free(c);
}


static int fork_handle(struct chrdev_handle *h, struct chrdev_client *child)
{
	struct chrdev_handle *copy = malloc(sizeof *copy);
	if(copy == NULL) return -ENOMEM;

	*copy = *h;
	copy->owner = child;
	copy->blocker = L4_nilthread;
	copy->flags &= ~HF_WRITE_BLOCKED;

	bool ok = htable_add(&handle_ht, rehash_handle(copy, NULL), copy);
	if(!ok) {
		free(copy);
		return -ENOMEM;
	}

	darray_push(child->handles, copy);
	copy->client_ix = child->handles.size - 1;
	assert(copy->owner->handles.item[copy->client_ix] == copy);

	darray_push(copy->file->handles, copy);
	copy->file_ix = copy->file->handles.size - 1;
	assert(copy->file->handles.item[copy->file_ix] == copy);

	return 0;
}


/* TODO: generate -ENOMEM, propagate failure as appropriate. the main problem
 * is that since forks are signaled through the sysmsg lifecycle protocol,
 * there's not much that we can do if this part of forking fails. presumably
 * we'll just scream into dmesg, leave some FDs invalid, cross fingers, and
 * hope for the best.
 */
static int fork_client(struct chrdev_client *parent, pid_t child_pid)
{
	size_t hash = int_hash(child_pid);
	if(htable_get(&client_ht, hash, &cmp_client_to_pid, &child_pid) != NULL) {
		fprintf(stderr, "chrdev[%d]: can't fork p=%d because c=%d exists!\n",
			getpid(), parent->pid, child_pid);
		return -EEXIST;
	}

	struct chrdev_client *c = malloc(sizeof *c);
	*c = (struct chrdev_client){
		.pid = child_pid,
		.handles = darray_new(),
		.next_fd = parent->next_fd,
	};
	darray_realloc(c->handles, parent->handles.size);
	assert(L4_IsNilThread(c->notify_tid));
	for(int i=0; i < parent->handles.size; i++) {
		int n = fork_handle(parent->handles.item[i], c);
		if(n < 0) {
			fprintf(stderr, "fork failure! TODO: handle this!\n");
			/* TODO: handle this without crapping out! */
			abort();
		}
	}
	assert(c->handles.size == parent->handles.size);
	bool ok = htable_add(&client_ht, hash, c);
	assert(ok);

	return 0;
}


static bool fd_exists(struct chrdev_client *c, uint16_t fd)
{
	size_t hash = int_hash(c->pid << 16 | fd);
	struct htable_iter it;
	for(struct chrdev_handle *cand = htable_firstval(&handle_ht, &it, hash);
		cand != NULL; cand = htable_nextval(&handle_ht, &it, hash))
	{
		if(cand->fd == fd && cand->owner == c) return true;
	}
	return false;
}


static int new_fd(struct chrdev_client *c)
{
	int fd, iters = 0;
	do {
		fd = c->next_fd++;
		if(fd == USHRT_MAX) c->next_fd = 1;
	} while(fd_exists(c, fd) && ++iters < USHRT_MAX);
	if(iters == USHRT_MAX) return -ENFILE;

	return fd;
}


static struct chrdev_handle *new_handle(
	struct chrdev_file *file, struct chrdev_client *c)
{
	struct chrdev_handle *h = malloc(sizeof *h);
	if(h == NULL) return NULL;
	*h = (struct chrdev_handle){
		.fd = new_fd(c), .owner = c, .file = file,
		.client_ix = c->handles.size, .file_ix = file->handles.size,
		.blocker = L4_nilthread,
	};
	if(h->fd < 0) {
		/* TODO: return ENFILE w/ an ERR_PTR() system */
		free(h);
		return NULL;
	}

	bool ok = htable_add(&handle_ht, rehash_handle(h, NULL), h);
	if(!ok) { free(h); return NULL; }

	darray_push(c->handles, h);	/* TODO: catch ENOMEM */
	assert(c->handles.item[h->client_ix] == h);
	darray_push(file->handles, h);
	assert(file->handles.item[h->file_ix] == h);

	return h;
}


/* for use in chrdev_confirm(), which shouldn't cause a cascade of lifecycle
 * handling just because it necessarily always resolves file descriptors.
 */
static struct chrdev_handle *get_handle_nosync(
	size_t *hash_p, pid_t pid, uint16_t fd)
{
	size_t hash = int_hash(pid << 16 | fd);
	if(hash_p != NULL) *hash_p = hash;
	struct htable_iter it;
	for(struct chrdev_handle *cand = htable_firstval(&handle_ht, &it, hash);
		cand != NULL; cand = htable_nextval(&handle_ht, &it, hash))
	{
		if(cand->fd == fd && cand->owner->pid == pid) {
			return cand;
		}
	}
	return NULL;
}


static struct chrdev_handle *get_handle(
	size_t *hash_p, pid_t pid, uint16_t fd)
{
	consume_lc_queue();
	return get_handle_nosync(hash_p, pid, fd);
}


/* shorthand for simple operations */
static struct chrdev_handle *resolve_fd(uint16_t fd) {
	return get_handle(NULL, pidof_NP(muidl_get_sender()), fd);
}


/* drain the lifecycle message queue. */
static void consume_lc_queue(void)
{
	uint32_t ctrl = atomic_load(&lce_ctrl);
	if(ctrl & (1 << 20)) {
		/* TODO: add the "list_children" thing, program image timestamps, and
		 * so forth for manual synchronization when lifecycle events were
		 * lost.
		 */
		fprintf(stderr, "chrdev[%d]: lost lifecycle events!\n", getpid());
		abort();
	}
	if(ctrl >> 10 == (ctrl & 0x3ff)) return;	/* empty */

	int first = ctrl >> 10, count = (int)(ctrl & 0x3ff) - first;
	if(count < 0) count += NUM_LC_EVENTS;
	assert(count > 0);
	for(int i=0; i < count; i++) {
		const struct lc_event *cur = &lce_queue[(first + i) % NUM_LC_EVENTS];
		pid_t p = cur->primary;
		struct chrdev_client *c = htable_get(&client_ht, int_hash(p),
			&cmp_client_to_pid, &p);
		if(c == NULL) {
			/* spurious, which is ok */
			continue;
		}
		switch(cur->tag) {
			case MPL_FORK:
				//printf("%s: forked %d -> %d\n", __func__, cur->primary, cur->child);
				fork_client(c, cur->child);
				break;
			case MPL_EXEC:
				//printf("%s: exec'd %d\n", __func__, cur->primary);
				break;
			case MPL_EXIT:
				//printf("%s: exited %d\n", __func__, cur->primary);
				client_dtor(c);
				sysmsg_rm_filter(lifecycle_msg, &(L4_Word_t){ p }, 1);
				break;
			default:
				printf("chrdev[%d]: weird lifecycle tag=%d\n",
					getpid(), cur->tag);
		}
	}

	uint32_t newctrl;
	do {
		assert(ctrl >> 10 == first);	/* single consumer */
		newctrl = (first + count) % NUM_LC_EVENTS << 10 | (ctrl & 0x3ff);
	} while(!atomic_compare_exchange_strong(&lce_ctrl, &ctrl, newctrl));
}


/* this adds and removes filters as appropriate, and records other events in
 * the event log. if the event log becomes full, filter adds are still
 * performed correctly, and the IDL dispatch thread gets its poke.
 */
static bool lifecycle_handler_fn(
	int bit, L4_Word_t *body, int body_len, void *priv)
{
	bool poke = false;
	uint32_t ctrl = atomic_load_explicit(&lce_ctrl, memory_order_consume);
	int wrpos = ctrl & 0x3ff;
	struct lc_event *ev = &lce_queue[wrpos], ev_dummy;
	if(ctrl & (1 << 20)) ev = &ev_dummy;	/* already in overflow */
	else if(ctrl >> 10 == (wrpos + 1) % NUM_LC_EVENTS) {
		/* ran out of events; mark overflow. */
		atomic_fetch_or_explicit(&lce_ctrl, 1 << 20, memory_order_relaxed);
		ev = &ev_dummy;
		poke = true;	/* try to resolve asap */
	}

	ev->tag = body[1] & 0xff;
	ev->primary = body[0];
	switch(ev->tag) {
		case MPL_FORK:
			ev->child = body[1] >> 8;
			sysmsg_add_filter(lifecycle_msg, &(L4_Word_t){ ev->child }, 1);
			break;
		case MPL_EXEC:
			/* nothing to add */
			break;
		case MPL_EXIT:
			ev->exit.signo = body[1] >> 8;
			ev->exit.waitstatus = body[2];
			ev->exit.exitcode = body[3];
			poke = true;
			break;
		default:
			printf("chrdev[%d]: unexpected lifecycle tag=%#x\n",
				getpid(), ev->tag);
	}

	uint32_t newctrl;
	do {
		assert((ctrl & 0x3ff) == wrpos);	/* single producer */
		newctrl = (ctrl & (0x3ff << 10)) | (wrpos + 1) % NUM_LC_EVENTS;
	} while(!atomic_compare_exchange_strong(&lce_ctrl, &ctrl, newctrl)
		&& (~ctrl & (1 << 20)));

	if(poke) {
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
		L4_LoadMR(1, main_tid.raw);
		L4_LoadMR(2, 0xbaab);
		L4_MsgTag_t tag = L4_Reply(thrd_to_tid(poke_thrd));
		if(L4_IpcFailed(tag) && L4_ErrorCode() != 2) {
			fprintf(stderr, "chrdev[%d]: failed lifecycle poke, ec=%lu\n",
				getpid(), L4_ErrorCode());
			/* causes delayed exit processing, i.e. SIGPIPE etc. are only
			 * raised in response to something else forcing the event queue.
			 *
			 * see the comment above poke_fn about IPC failure; in practice
			 * this isn't supposed to happen, but we'll report it anyhow.
			 */
		}
	}

	/* effects are immediate enough, i.e. the queue is forced before any
	 * client can tell the difference.
	 */
	return true;
}


static struct chrdev_client *caller(bool create)
{
	consume_lc_queue();
	pid_t pid = pidof_NP(muidl_get_sender());
	size_t hash = int_hash(pid);
	struct chrdev_client *c = htable_get(&client_ht, hash,
		&cmp_client_to_pid, &pid);
	if(c != NULL || !create) return c;

	c = malloc(sizeof *c);
	if(c == NULL) return NULL;

	static bool first_client = true;
	if(first_client) {
		first_client = false;
		lifecycle_msg = sysmsg_listen(MSGB_PROCESS_LIFECYCLE,
			&lifecycle_handler_fn, NULL);
		if(lifecycle_msg < 0) {
			fprintf(stderr,
				"chrdev[%d]: can't listen to process lifecycle, n=%d\n",
				getpid(), lifecycle_msg);
			abort();
		}
	}
	sysmsg_add_filter(lifecycle_msg, &(L4_Word_t){ pid }, 1);
	*c = (struct chrdev_client){
		.pid = pid, .next_fd = 1, .handles = darray_new(),
	};
	bool ok = htable_add(&client_ht, hash, c);
	assert(ok);
	return c;
}


/* thread that converts an "edge" poke into a "level" event. without causing
 * the sysmsg handler to block, this ensures that MPL_EXIT is processed right
 * away so that SIGPIPE and the like go out immediately in response to peer
 * exit.
 *
 * atomicity is guaranteed by microkernel Ipc syscall, so the thread is always
 * either waiting to receive, waiting to send, or on its way to send; so an
 * edge-mode poke will either get merged into an existing send or generate one
 * where there wasn't before.
 *
 * this isn't robust at all against IPC failure, which shouldn't occur unless
 * ExchangeRegisters interrupts one of the sleeping sides. in that case
 * signals may be lost either due to a broken sleeping send, or edge pokes
 * coming in between failure handling and the non-send wait in the outer loop.
 * the latter may be solved by running this thread on a priority band above
 * its clients.
 */
static int poke_fn(void *param_ptr)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_Never, &sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				fprintf(stderr, "chrdev[%d]:%s: ipc failed, ec=%lu\n",
					getpid(), __func__, L4_ErrorCode());
				break;
			}
			L4_ThreadId_t dest;
			L4_Word_t label;
			L4_StoreMR(1, &dest.raw);
			L4_StoreMR(2, &label);
			L4_Accept(L4_UntypedWordsAcceptor);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.label = label }.raw);
			/* "LsendWaitLocal" */
			tag = L4_Lipc(dest, L4_anylocalthread,
				L4_Timeouts(L4_Never, L4_Never), &sender);
		}
	}
	assert(false);
}


static void spawn_poke_thrd(void)
{
	int n = thrd_create(&poke_thrd, &poke_fn, NULL);
	if(n != thrd_success) {
		printf("chrdev[%d]: can't create poke thread, n=%d\n", getpid(), n);
		abort();
	}

	/* this puts poke_fn() into the "LreplyWaitLocal" form's receive phase,
	 * which means it's got past the initialization part where it might miss
	 * an early edge poke.
	 */
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
	L4_LoadMR(1, L4_MyLocalId().raw);
	L4_LoadMR(2, 0xbe7a);
	L4_MsgTag_t tag = L4_Lcall(thrd_to_tid(poke_thrd));
	if(L4_IpcFailed(tag)) {
		printf("chrdev[%d]: can't sync with poke thread, ec=%lu\n",
			getpid(), L4_ErrorCode());
		abort();
	}
}


/* IO::set_notify events. */
static void send_poll_event(struct chrdev_handle *r, int events)
{
	if((r->events & events) == 0) return;
	L4_ThreadId_t ntid = r->owner->notify_tid;
	if(L4_IsNilThread(ntid)) {
		printf("chrdev: pid=%d notify_tid is nil!\n",
			r->owner->pid);
		return;
	}
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = my_pid, .X.u = 2 }.raw);
	L4_LoadMR(1, events);
	L4_LoadMR(2, r->fd);
	L4_MsgTag_t tag = L4_Reply(ntid);
	if(L4_IpcFailed(tag)) {
		L4_Word_t ec = L4_ErrorCode();
		if(ec != 2) {
			printf("chrdev: %s: weird ec=%lu\n", __func__, ec);
			return;
		}
		/* receiver not ready; force sync thru SIGIO. */
		int n = __proc_kill(__uapi_tid, r->owner->pid, SIGIO);
		if(n != 0) {
			printf("chrdev: Proc::kill[SIGIO] to pid=%d failed, n=%d\n",
				r->owner->pid, n);
			/* ... and do nothing.
			 * TODO: do something?
			 */
		}
	}
	printf("chrdev: notification to %lu:%lu %s\n",
		L4_ThreadNo(ntid), L4_Version(ntid),
		L4_IpcFailed(tag) ? "failed" : "succeeded");
}


/* TODO: notify multiple threads using the blocker_hash (or some such) once
 * userspace gets multithreading in the vague, distant future.
 */
void chrdev_notify(chrfile_t *impl, int mask)
{
	assert(impl != NULL);

	struct chrdev_file *f = CHR_FILE(impl);
	L4_ThreadId_t sent[f->handles.size];	/* try at most once each. */
	int n_sent = 0;
	struct chrdev_handle **h_it;
	darray_foreach(h_it, f->handles) {
		struct chrdev_handle *h = *h_it;
		send_poll_event(h, mask);
		if(L4_IsNilThread(h->blocker)) continue;
		bool found = false;
		for(int i=0; i < n_sent; i++) {
			if(sent[i].raw == h->blocker.raw) {
				found = true;
				break;
			}
		}
		if(found) continue;
		if((mask & (EPOLLHUP | EPOLLERR))
			|| ((h->flags & HF_WRITE_BLOCKED) && (mask & EPOLLOUT))
			|| ((~h->flags & HF_WRITE_BLOCKED) && (mask & EPOLLIN)))
		{
			assert(pidof_NP(h->blocker) == h->owner->pid);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
			L4_LoadMR(1, EAGAIN);
			L4_MsgTag_t tag = L4_Reply(h->blocker);
			if(L4_IpcSucceeded(tag)) {
				assert(n_sent < f->handles.size);
				sent[n_sent++] = h->blocker;
				h->blocker = L4_nilthread;
			}
		}
	}
}


static int chrdev_set_flags(int *old, int fd, int or, int and)
{
	const unsigned permitted_flags = SNEKS_IO_O_NONBLOCK;
	and |= ~permitted_flags;
	or &= permitted_flags;

	struct chrdev_handle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;

	*old = h->flags;
	h->flags = (*old & and) | or | (*old & ~permitted_flags);

	return 0;
}


static void chrdev_confirm(L4_Word_t param, void *priv)
{
	pid_t pid = (uintptr_t)priv >> 16;
	int fd = (uintptr_t)priv & 0xffff;

	struct chrdev_handle *h = get_handle_nosync(NULL, pid, fd);
	if(h == NULL) {
		fprintf(stderr, "chrdev[%d]:%s: handle not found for pid=%d, fd=%d?\n",
			getpid(), __func__, pid, fd);
		return;
	}
	(*callbacks.confirm)(CHR_IMPL(h->file),
		param & 0x7fffffff, !!(param & 0x80000000));
}


/* encode the param/priv format for read and write calls. consumed by
 * chrdev_confirm(), up there.
 */
static void wr_confirm(bool writing, int fd, unsigned count)
{
	assert((fd & 0xffff) == fd);
	pid_t pid = pidof_NP(muidl_get_sender());
	set_confirm(&chrdev_confirm,
		(writing ? 0x80000000 : 0) | (count & 0x7fffffff),
		(void *)((fd & 0xffff) | (((uintptr_t)pid & 0xffff) << 16)));
}


static int chrdev_write(
	int fd, off_t offset, const uint8_t *buf, unsigned count)
{
	sync_confirm();
	struct chrdev_handle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;
	if(offset != -1) return -EBADF;	/* not seekable */

	/* clamp count to USHRT_MAX because IO::write returns unsigned short. this
	 * is kind of bad and should get fixed at some point.
	 */
	int n = (*callbacks.write)(CHR_IMPL(h->file), buf,
		min_t(unsigned, USHRT_MAX, count));
	if(n == -EWOULDBLOCK && (~h->flags & HF_NONBLOCK)) {
		if(!L4_IsNilThread(h->blocker)) {
			/* TODO: have an affordance for when a multithreaded userspace has
			 * several threads sleeping on a single character device file.
			 */
			fprintf(stderr, "%s: overwriting existing blocker!\n", __func__);
		}
		h->blocker = muidl_get_sender();
		h->flags |= HF_WRITE_BLOCKED;
		muidl_raise_no_reply();
		return 0;
	} else {
		if(n > 0) wr_confirm(true, fd, n);
		return n;
	}
}


static int chrdev_read(int fd, int count, off_t offset,
	uint8_t *buf, unsigned *buf_len_p)
{
	sync_confirm();
	struct chrdev_handle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;
	if(offset != -1) return -EBADF;	/* not seekable */

	int n = (*callbacks.read)(CHR_IMPL(h->file), buf, count);
	if(n == -EWOULDBLOCK && (~h->flags & HF_NONBLOCK)) {
		if(!L4_IsNilThread(h->blocker)) {
			/* TODO: see same in chrdev_write() */
			fprintf(stderr, "%s: overwriting existing blocker!\n", __func__);
		}
		h->blocker = muidl_get_sender();
		h->flags &= ~HF_WRITE_BLOCKED;
		muidl_raise_no_reply();
		return 0;
	} else {
		*buf_len_p = max(n, 0);
		if(n > 0) wr_confirm(false, fd, n);
		return min(n, 0);
	}
}


static int chrdev_close(int fd)
{
	sync_confirm();
	struct chrdev_handle *h = resolve_fd(fd);
	return h == NULL ? -EBADF : handle_dtor(h);
}


static int chrdev_dup(int *newfd_p, int oldfd)
{
	sync_confirm();

	struct chrdev_client *c = caller(false);
	struct chrdev_handle *h = resolve_fd(oldfd);
	if(c == NULL || h == NULL) return -EBADF;

	struct chrdev_handle *copy = new_handle(h->file, c);
	if(copy == NULL) return -ENOMEM;

	*newfd_p = copy->fd;
	return 0;
}


static int chrdev_set_notify(int *exmask_p,
	int fd, int events, L4_Word_t notify_tid_raw)
{
	sync_confirm();
	if(notify_tid_raw != L4_nilthread.raw) {
		struct chrdev_client *c = caller(false);
		if(c == NULL) return -EBADF;	/* fd can't be valid either */
		L4_ThreadId_t tid = { .raw = notify_tid_raw };
		if(!L4_IsGlobalId(tid)) return -EINVAL;
		c->notify_tid = tid;
	}

	struct chrdev_handle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;

	h->events = events;
	*exmask_p = (*callbacks.get_status)(CHR_IMPL(h->file)) & (events | EPOLLHUP);
	return 0;
}


static void chrdev_get_status(
	const L4_Word_t *handles, unsigned n_handles,
	const uint16_t *notif, unsigned n_notif,
	L4_Word_t *st, unsigned *n_st_p)
{
	sync_confirm();
	pid_t spid = pidof_NP(muidl_get_sender());
	for(int i=0; i < n_handles; i++) {
		struct chrdev_handle *h = get_handle(NULL, spid, handles[i]);
		st[i] = h == NULL ? ~0ul : (*callbacks.get_status)(CHR_IMPL(h->file));
		if(h != NULL && i < n_notif) h->events = notif[i];
	}
	*n_st_p = n_handles;
}


/* Sneks::Pipe calls */

static int chrdev_pipe(L4_Word_t *rd_p, L4_Word_t *wr_p, int flags)
{
	sync_confirm();

	if(flags != 0) return -EINVAL;

	int n;
	struct chrdev_client *c = caller(true);
	struct chrdev_file *readf = malloc(sizeof *readf + impl_size),
		*writef = malloc(sizeof *writef + impl_size);
	if(c == NULL || readf == NULL || writef == NULL) goto Enomem;
	*readf = (struct chrdev_file){ .handles = darray_new() };
	*writef = (struct chrdev_file){ .handles = darray_new() };

	n = (*callbacks.pipe)(CHR_IMPL(readf), CHR_IMPL(writef), flags);
	if(n < 0) goto fail;

	struct chrdev_handle *rdh = new_handle(readf, c), *wrh = new_handle(writef, c);
	if(wrh == NULL || rdh == NULL) {
		if(wrh == NULL) (*callbacks.close)(CHR_IMPL(writef));
		assert(rdh == NULL || readf->handles.size == 1);
		if(rdh != NULL) { handle_dtor(rdh); readf = NULL; }
		goto Enomem;
	}

	*rd_p = rdh->fd;
	*wr_p = wrh->fd;
	return 0;

Enomem: n = -ENOMEM;
fail:
	free(readf); free(writef);
	assert(n < 0);
	return n;
}


/* Sneks::DeviceNode calls */

static int chrdev_open(int *handle_p,
	uint32_t object, L4_Word_t cookie, int flags)
{
	sync_confirm();

	/* (we'll ignore @cookie because UAPI will have already checked it for us,
	 * and we don't have the key material anyway.)
	 */

	/* TODO: use these somewhere? */
	flags &= ~(O_RDONLY | O_WRONLY | O_RDWR);
	if(flags != 0) return -EINVAL;

	struct chrdev_client *c = caller(true);
	struct chrdev_file *file = malloc(sizeof *file + impl_size);
	if(c == NULL || file == NULL) { free(file); return -ENOMEM; }
	*file = (struct chrdev_file){ .handles = darray_new() };

	static const char objtype[] = { [2] = 'c' };
	int n = (*callbacks.dev_open)(CHR_IMPL(file), objtype[(object >> 30) & 0x3],
		(object >> 15) & 0x7fff, object & 0x7fff, flags);
	if(n < 0) { free(file); return n; }

	struct chrdev_handle *h = new_handle(file, c);
	if(h == NULL) {
		(*callbacks.close)(CHR_IMPL(file));
		return -ENOMEM;
	}

	*handle_p = h->fd;
	return 0;
}


static int chrdev_ioctl_void(int *result_p, int handle, unsigned request)
{
	/* TODO */
	return -ENOSYS;
}


static int chrdev_ioctl_int(int *result_p,
	int handle, unsigned request, int *arg_p)
{
	/* TODO */
	return -ENOSYS;
}


static int enosys() {
	/* straight up drink a glass of milk */
	return -ENOSYS;
}


int chrdev_run(size_t sizeof_file, int argc, char *argv[])
{
	/* TODO: surely there's a better way to keep HF_* and SNEKS_IO_O_*
	 * separate. for now there's just one of each so it's this easy.
	 */
	assert(HF_WRITE_BLOCKED != HF_NONBLOCK);

	impl_size = sizeof_file;
	main_tid = L4_MyLocalId();
	my_pid = getpid();
	spawn_poke_thrd();
	htable_init(&client_ht, &rehash_client, NULL);
	htable_init(&handle_ht, &rehash_handle, NULL);

	static const struct chrdev_impl_vtable vtab = {
		/* Sneks::IO */
		.set_flags = &chrdev_set_flags,
		.write = &chrdev_write,
		.read = &chrdev_read,
		.close = &chrdev_close,
		.dup = &chrdev_dup,
		.dup_to = &enosys, .touch = &enosys,

		/* Sneks::Poll */
		.set_notify = &chrdev_set_notify,
		.get_status = &chrdev_get_status,

		/* Sneks::Pipe */
		.pipe = &chrdev_pipe,

		/* Sneks::DeviceNode */
		.open = &chrdev_open,
		.ioctl_void = &chrdev_ioctl_void,
		.ioctl_int = &chrdev_ioctl_int,
	};

	for(;;) {
		L4_Word_t status = _muidl_chrdev_impl_dispatch(&vtab);
		L4_ThreadId_t sender = muidl_get_sender();
		L4_MsgTag_t tag = muidl_get_tag();
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)
			&& selftest_handling(status))
		{
			/* oof */
		} else if(L4_IsLocalId(sender) && L4_Label(tag) == 0xbaab) {
			/* queue-consumption stimulus, i.e. exit was signaled and some I/O
			 * notifications may fire.
			 */
			consume_lc_queue();
		} else {
			printf("chrdev: dispatch status %#lx (last tag %#lx)\n",
				status, tag.raw);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 1 }.raw);
			L4_LoadMR(1, ENOSYS);
			L4_Reply(sender);
		}
	}
}
