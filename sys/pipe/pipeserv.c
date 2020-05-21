
#define PIPESERVIMPL_IMPL_SOURCE
#undef BUILD_SELFTEST

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <ccan/list/list.h>
#include <ccan/darray/darray.h>
#include <ccan/htable/htable.h>
#include <ccan/minmax/minmax.h>
#include <ccan/membuf/membuf.h>

#include <sneks/hash.h>
#include <sneks/msg.h>
#include <sneks/thread.h>
#include <sneks/process.h>
#include <sneks/systask.h>

#include "muidl.h"
#include "io-defs.h"
#include "proc-defs.h"
#include "pipeserv-impl-defs.h"


/* due to the way CCAN membuf works, it may be the case that pipes appear full
 * with only half of PIPESZ available to read. this should still be enough,
 * and there'll usually be room for more.
 */
#define PIPESZ 4096
#define NUM_LC_EVENTS 128	/* at most 1024 */

#define PHF_WRITER (O_WRONLY << 16)	/* if not reader */
#define PHF_NONBLOCK (SNEKS_IO_O_NONBLOCK << 16)

/* NOTE: IS_FULL() may be invalid until membuf_prepare_space(&(hd)->buf, 1)
 * has run.
 */
#define IS_FULL(hd) (membuf_num_space(&(hd)->buf) == 0)
#define IS_EMPTY(hd) (membuf_num_elems(&(hd)->buf) == 0)


struct pipeclient;

struct pipehead
{
	/* NOTE: ideally we'd have a more cleverer VM ringbuffer here, and muidl's
	 * dispatch stubs would let strxfer right out of said buffer without an
	 * extra memcpy; but that can wait until tests become good enough to trust
	 * something so fancy.
	 */
	MEMBUF(char) buf;
	struct list_head readh, writeh;	/* of <struct pipehandle> */
	darray(L4_ThreadId_t) sleepers;	/* blocking clients */
};


struct pipehandle
{
	/* link in ->head's readh or writeh, per PHF_WRITER in ->bits. */
	struct list_node ph_link;
	struct pipehead *head;
	struct pipeclient *owner;
	int bits;		/* low 16 are fd, rest are PHF_* */
	int client_ix;	/* index in pipeclient.handles */
	uint32_t events;/* per IO::set_notify */
};


struct pipeclient
{
	pid_t pid;
	L4_ThreadId_t notify_tid;
	unsigned short next_bits;
	darray(struct pipehandle *) handles;
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


static L4_ThreadId_t main_tid;
static thrd_t poke_thrd;
static struct htable client_ht, handle_ht;
static int my_pid;
static int lifecycle_msg = -1;	/* sysmsg handle */
/* an ad-hoc SPSC concurrent circular buffer. */
static struct lc_event lce_queue[NUM_LC_EVENTS];
/* its control word. 0..9 = read pos, 10..19 = write pos, 20 = overflow */
static _Atomic uint32_t lce_ctrl;


static void wakey_wakey(struct pipehead *hd, int code, int pid);
static void send_notify(struct list_head *handle_list, int events);


static size_t rehash_pipeclient(const void *ptr, void *priv) {
	const struct pipeclient *c = ptr;
	return int_hash(c->pid);
}


static size_t rehash_pipehandle(const void *ptr, void *priv) {
	const struct pipehandle *h = ptr;
	return int_hash(h->owner->pid << 16 | (h->bits & 0xffff));
}


static bool cmp_client_to_pid(const void *cand, void *key) {
	const struct pipeclient *c = cand;
	return c->pid == *(pid_t *)key;
}


static void pipehandle_dtor(struct pipehandle *h)
{
	/* disconnect the pipe head. */
	struct list_head *hh = h->bits & PHF_WRITER
		? &h->head->writeh : &h->head->readh;
	list_del_from(hh, &h->ph_link);
	if(list_empty(&h->head->writeh) && list_empty(&h->head->readh)) {
		/* pipe goes away. */
		/* NOTE: this wakey_wakey() isn't necessary; if all read and write
		 * handles to that head have been deleted, then all sleepers will be
		 * invalid and can be just discarded. TODO: do that.
		 */
		wakey_wakey(h->head, EBADF, h->owner->pid);
		darray_free(h->head->sleepers);
		free(membuf_cleanup(&h->head->buf));
		free(h->head);
	} else if(list_empty(&h->head->writeh)) {
		wakey_wakey(h->head, EAGAIN, -1);
		send_notify(&h->head->readh, EPOLLHUP);
	}

	/* remove handle from client and htable, then dispose. */
	struct pipeclient *c = h->owner;
	assert(h->client_ix < c->handles.size);
	assert(c->handles.item[h->client_ix] == h);
	struct pipehandle *last = c->handles.item[c->handles.size - 1];
	last->client_ix = h->client_ix;
	c->handles.item[h->client_ix] = last;
	darray_resize(c->handles, c->handles.size - 1);
	bool ok = htable_del(&handle_ht, rehash_pipehandle(h, NULL), h);
	assert(ok);
	free(h);
}


static void pipeclient_dtor(struct pipeclient *c)
{
	while(c->handles.size > 0) {
		pipehandle_dtor(c->handles.item[0]);
	}
	bool ok = htable_del(&client_ht, int_hash(c->pid), c);
	assert(ok);
	darray_free(c->handles);
	free(c);
}


static void add_handle(struct pipehead *hd, struct pipehandle *handle) {
	list_add_tail(handle->bits & PHF_WRITER ? &hd->writeh : &hd->readh,
		&handle->ph_link);
}


/* one of two constructors of <struct pipehandle>. pipe_pipe() is the
 * other.
 */
static void fork_pipehandle(
	const struct pipehandle *h, struct pipeclient *child)
{
	struct pipehandle *copy = malloc(sizeof *copy);
	copy->head = h->head;
	copy->owner = child;
	copy->bits = h->bits;
	copy->events = h->events;
	darray_push(child->handles, copy);
	copy->client_ix = child->handles.size - 1;
	assert(copy->client_ix == h->client_ix);
	add_handle(copy->head, copy);
	bool ok = htable_add(&handle_ht, rehash_pipehandle(copy, NULL), copy);
	assert(ok);
}


/* one of two constructors of <struct pipeclient>. caller() is the other. */
static void fork_pipeclient(struct pipeclient *parent, pid_t child_pid)
{
	size_t hash = int_hash(child_pid);
	if(htable_get(&client_ht, hash, &cmp_client_to_pid, &child_pid) != NULL) {
		fprintf(stderr, "pipeserv: can't fork p=%d because c=%d exists!\n",
			parent->pid, child_pid);
		return;
	}

	struct pipeclient *c = malloc(sizeof *c);
	*c = (struct pipeclient){
		.pid = child_pid,
		.handles = darray_new(),
		.next_bits = parent->next_bits,
	};
	darray_realloc(c->handles, parent->handles.size);
	for(int i=0; i < parent->handles.size; i++) {
		fork_pipehandle(parent->handles.item[i], c);
	}
	assert(c->handles.size == parent->handles.size);
	bool ok = htable_add(&client_ht, hash, c);
	assert(ok);
}


static void consume_lc_queue(void)
{
	uint32_t ctrl = atomic_load(&lce_ctrl);
	if(ctrl & (1 << 20)) {
		/* FIXME: add the "list_children" thing, program image timestamps, and
		 * so forth for manual synchronization when lifecycle events were
		 * lost.
		 */
		fprintf(stderr, "pipeserv: lifecycle events were lost!\n");
		abort();
	}
	if(ctrl >> 10 == (ctrl & 0x3ff)) return;	/* empty */

	int first = ctrl >> 10, count = (int)(ctrl & 0x3ff) - first;
	if(count < 0) count += NUM_LC_EVENTS;
	assert(count > 0);
	for(int i=0; i < count; i++) {
		const struct lc_event *cur = &lce_queue[(first + i) % NUM_LC_EVENTS];
		pid_t p = cur->primary;
		struct pipeclient *c = htable_get(&client_ht, int_hash(p),
			&cmp_client_to_pid, &p);
		if(c == NULL) {
			/* spurious, which is ok */
			continue;
		}
		switch(cur->tag) {
			case MPL_FORK:
				//printf("%s: forked %d -> %d\n", __func__, cur->primary, cur->child);
				fork_pipeclient(c, cur->child);
				break;
			case MPL_EXEC:
				//printf("%s: exec'd %d\n", __func__, cur->primary);
				break;
			case MPL_EXIT:
				//printf("%s: exited %d\n", __func__, cur->primary);
				pipeclient_dtor(c);
				sysmsg_rm_filter(lifecycle_msg, &(L4_Word_t){ p }, 1);
				break;
			default:
				printf("pipeserv: weird lifecycle tag=%d\n", cur->tag);
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
			printf("pipeserv: unexpected lifecycle tag=%#x\n", ev->tag);
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
			fprintf(stderr, "pipeserv: failed lifecycle poke, ec=%lu\n",
				L4_ErrorCode());
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
				fprintf(stderr, "pipeserv:%s: ipc failed, ec=%lu\n",
					__func__, L4_ErrorCode());
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


/* one of two constructors of <struct pipeclient>. fork_pipeclient() is the
 * other.
 */
static struct pipeclient *caller(bool create)
{
	consume_lc_queue();
	pid_t pid = pidof_NP(muidl_get_sender());
	size_t hash = int_hash(pid);
	struct pipeclient *c = htable_get(&client_ht, hash,
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
				"pipeserv: can't listen to process lifecycle, n=%d\n",
				lifecycle_msg);
			abort();
		}
	}
	sysmsg_add_filter(lifecycle_msg, &(L4_Word_t){ pid }, 1);
	*c = (struct pipeclient){
		.pid = pid,
		.handles = darray_new(),
		.next_bits = 1,
	};
	bool ok = htable_add(&client_ht, hash, c);
	assert(ok);
	return c;
}


static void client_dtor(struct pipeclient *c)
{
	assert(c->handles.size == 0);
	darray_free(c->handles);
	htable_del(&client_ht, rehash_pipeclient(c, NULL), c);
	free(c);
}


static bool bits_exist(struct pipeclient *c, int bits)
{
	bits &= 0xffff;
	size_t hash = int_hash(c->pid << 16 | bits);
	struct htable_iter it;
	for(struct pipehandle *cand = htable_firstval(&handle_ht, &it, hash);
		cand != NULL;
		cand = htable_nextval(&handle_ht, &it, hash))
	{
		if((cand->bits & 0xffff) == bits && cand->owner == c) return true;
	}
	return false;
}


static int new_bits(struct pipeclient *c)
{
	int bits, iters = 0;
	do {
		bits = c->next_bits++;
		if(bits == USHRT_MAX) {
			bits = 1;
			c->next_bits = 2;
		}
	} while(bits_exist(c, bits) && iters < USHRT_MAX - 1);
	if(iters == USHRT_MAX - 1) return -ENOMEM; /* TODO: better error code? */

	return bits;
}


static struct pipehandle *get_handle(size_t *hash_p, pid_t pid, int fd)
{
	consume_lc_queue();
	fd &= 0xffff;
	size_t hash = int_hash(pid << 16 | fd);
	if(hash_p != NULL) *hash_p = hash;
	struct htable_iter it;
	for(struct pipehandle *cand = htable_firstval(&handle_ht, &it, hash);
		cand != NULL;
		cand = htable_nextval(&handle_ht, &it, hash))
	{
		if((cand->bits & 0xffff) == fd && cand->owner->pid == pid) {
			return cand;
		}
	}
	return NULL;
}


static struct pipehandle *resolve_fd(int fd) {
	return get_handle(NULL, pidof_NP(muidl_get_sender()), fd);
}


static void *membuf_snub(struct membuf *mb, void *rawptr, size_t newsize) {
	/* let's not resize pipes all willy-nilly. */
	return NULL;
}


/* one of two constructors of <struct pipeclient>. fork_pipehandle() is the
 * other.
 */
static int pipe_pipe(L4_Word_t *rd_p, L4_Word_t *wr_p, int flags)
{
	if(flags != 0) return -EINVAL;

	struct pipehead *hd = malloc(sizeof *hd);
	if(hd == NULL) return -ENOMEM;
	char *firstbuf = aligned_alloc(4096, PIPESZ);
	if(firstbuf == NULL) {
		free(hd);
		return -ENOMEM;
	}

	struct pipehandle *readh = malloc(sizeof *readh),
		*writeh = malloc(sizeof *writeh);
	if(readh == NULL || writeh == NULL) {
Enomem:
		free(firstbuf); free(hd); free(readh); free(writeh);
		return -ENOMEM;
	}

	struct pipeclient *c = caller(true);
	if(c == NULL) goto Enomem;
	darray_init(hd->sleepers);
	membuf_init(&hd->buf, firstbuf, PIPESZ, &membuf_snub);
	list_head_init(&hd->readh);
	list_head_init(&hd->writeh);
	*readh = (struct pipehandle){
		.owner = c, .head = hd,
		.bits = new_bits(c) & ~PHF_WRITER,
		.client_ix = c->handles.size,
	};
	*writeh = (struct pipehandle){
		.owner = c, .head = hd,
		.bits = new_bits(c) | PHF_WRITER,
		.client_ix = c->handles.size + 1,
	};
	if(readh->bits < 0 || writeh->bits < 0) {
		if(c->handles.size == 0) client_dtor(c);
		goto Enomem;	/* NOTE: weird error code */
	}
	add_handle(hd, readh);
	add_handle(hd, writeh);
	bool ok = htable_add(&handle_ht, rehash_pipehandle(readh, NULL), readh);
	ok = ok && htable_add(&handle_ht, rehash_pipehandle(writeh, NULL), writeh);
	assert(ok);		/* TODO: we can catch it so we should */
	darray_appends(c->handles, readh, writeh);	/* ... not this one though. */

	*rd_p = readh->bits & 0xffff;
	*wr_p = writeh->bits & 0xffff;
	return 0;
}


static int pipe_close(int fd)
{
	struct pipehandle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;
	else {
		pipehandle_dtor(h);
		return 0;
	}
}


static int pipe_set_flags(int *old, int fd, int or, int and)
{
	const unsigned permitted_flags = SNEKS_IO_O_NONBLOCK;
	and |= ~permitted_flags;
	or &= permitted_flags;

	struct pipehandle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;

	*old = h->bits >> 16;
	unsigned new = (*old & and) | or | (*old & ~permitted_flags);
	h->bits = (h->bits & 0xffff) | (new << 16);

	return 0;
}


/* return mask of EPOLL* representing the active level-triggered I/O status of
 * the pipe associated with @h.
 */
static uint32_t event_status(struct pipehandle *h)
{
	uint32_t st;
	if(h->bits & PHF_WRITER) {
		if(list_empty(&h->head->readh)) st = EPOLLERR;	/* EPIPE pending */
		else {
			membuf_prepare_space(&h->head->buf, 1);
			if(!IS_FULL(h->head)) st = EPOLLOUT;
			else st = 0;
		}
	} else {
		if(!IS_EMPTY(h->head)) st = EPOLLIN;
		else if(list_empty(&h->head->writeh)) st = EPOLLHUP;
		else st = 0;
	}
	return st;
}


static int pipe_set_notify(
	int *exmask_ptr, int fd, int events, L4_Word_t tid_raw)
{
	if(tid_raw != L4_nilthread.raw) {
		struct pipeclient *c = caller(false);
		if(c == NULL) return -EBADF;
		L4_ThreadId_t tid = { .raw = tid_raw };
		if(!L4_IsGlobalId(tid)) return -EINVAL;
		c->notify_tid = tid;
	}

	struct pipehandle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;
	h->events = events;
	*exmask_ptr = event_status(h) & (h->events | EPOLLHUP);
	return 0;
}


static void pipe_get_status(
	const L4_Word_t *handles, unsigned n_handles,
	L4_Word_t *st, unsigned *n_st_p)
{
	int spid = pidof_NP(muidl_get_sender());
	for(int i=0; i < n_handles; i++) {
		struct pipehandle *h = get_handle(NULL, spid, handles[i]);
		st[i] = h == NULL ? ~0ul : event_status(h);
	}
	*n_st_p = n_handles;
}


/* wakes up blocking sleepers. */
static void wakey_wakey(struct pipehead *hd, int code, int pid)
{
	for(int i=0; i < hd->sleepers.size; i++) {
		if(pid >= 0 && pidof_NP(hd->sleepers.item[i]) != pid) continue;
		L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
		L4_LoadMR(1, code);
		L4_Reply(hd->sleepers.item[i]);
		if(pid >= 0) {
			hd->sleepers.item[i] = hd->sleepers.item[hd->sleepers.size - 1];
			darray_resize(hd->sleepers, hd->sleepers.size - 1);
			i--;
		}
	}
	if(pid < 0) darray_resize(hd->sleepers, 0);
}


/* IO::set_notify interaction with structs pipehandle in @handle_list. */
static void send_notify(struct list_head *handle_list, int events)
{
	struct pipehandle *r;
	list_for_each(handle_list, r, ph_link) {
		if((r->events & events) == 0) continue;
		L4_ThreadId_t ntid = r->owner->notify_tid;
		assert(!L4_IsNilThread(ntid));
		L4_LoadMR(0, (L4_MsgTag_t){ .X.label = my_pid, .X.u = 2 }.raw);
		L4_LoadMR(1, events);
		L4_LoadMR(2, r->bits & 0xffff);
		L4_MsgTag_t tag = L4_Reply(ntid);
		if(L4_IpcFailed(tag)) {
			L4_Word_t ec = L4_ErrorCode();
			if(ec != 2) {
				printf("pipeserv: %s: weird ec=%lu\n", __func__, ec);
			} else {
				/* receiver not ready. */
				extern L4_ThreadId_t __uapi_tid;
				int n = __proc_kill(__uapi_tid, r->owner->pid, SIGIO);
				if(n != 0) {
					printf("pipeserv: Proc::kill[SIGIO] to pid=%d failed, n=%d\n",
						r->owner->pid, n);
					/* ... and do nothing.
					 * FIXME: do something?
					 */
				}
			}
		}
#if 0
		printf("%s: notification to %lu:%lu %s\n", __func__,
			L4_ThreadNo(ntid), L4_Version(ntid),
			L4_IpcFailed(tag) ? "failed" : "succeeded");
#endif
	}
}


/* this de-inlines darray_push(), which would otherwise compile into many
 * instructions within the write and read sides both.
 */
static void add_sleeper(struct pipehead *hd, L4_ThreadId_t tid) {
	darray_push(hd->sleepers, tid);
}


static int pipe_write(
	int fd, const uint8_t *buf, unsigned buf_len)
{
	struct pipehandle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;

	if(list_empty(&h->head->readh)) return -EPIPE;	/* bork'd */
	if(buf_len == 0) return 0;
	membuf_prepare_space(&h->head->buf, buf_len);
	bool was_empty = IS_EMPTY(h->head);
	if(IS_FULL(h->head)) {
		if(h->bits & PHF_NONBLOCK) return -EWOULDBLOCK;
		else {
			/* block until not full. */
			add_sleeper(h->head, muidl_get_sender());
			muidl_raise_no_reply();
			return 0;
		}
	}

	size_t written = min(buf_len, membuf_num_space(&h->head->buf));
	memcpy(membuf_space(&h->head->buf), buf, written);
	if(was_empty) {
		wakey_wakey(h->head, EAGAIN, -1);
		send_notify(&h->head->readh, EPOLLIN);
	}

	/* (TODO: move this into a confirm callback, ere long) */
	membuf_added(&h->head->buf, written);

	return written;
}


static int pipe_read(
	int fd, uint32_t length,
	uint8_t *buf, unsigned *buf_len_p)
{
	struct pipehandle *h = resolve_fd(fd);
	if(h == NULL) return -EBADF;

	if(IS_EMPTY(h->head)) {
		if(!list_empty(&h->head->writeh)) {
			if(h->bits & PHF_NONBLOCK) return -EWOULDBLOCK;
			else {
				/* block until not empty. */
				add_sleeper(h->head, muidl_get_sender());
				muidl_raise_no_reply();
				return 0;
			}
		} else {
			/* closed & drained, i.e. EOF */
			*buf_len_p = 0;
			return 0;
		}
	}

	membuf_prepare_space(&h->head->buf, 1);
	bool was_full = IS_FULL(h->head);
	size_t got = min(length, membuf_num_elems(&h->head->buf));
	memcpy(buf, membuf_elems(&h->head->buf), got);
	if(got > 0 && was_full) {
		wakey_wakey(h->head, EAGAIN, -1);
		send_notify(&h->head->writeh, EPOLLOUT);
	}

	/* TODO: move this into a confirm callback */
	membuf_consume(&h->head->buf, got);

	*buf_len_p = got;
	return got;
}


static void spawn_poke_thrd(void)
{
	int n = thrd_create(&poke_thrd, &poke_fn, NULL);
	if(n != thrd_success) {
		printf("pipeserv: can't create poke thread, n=%d\n", n);
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
		printf("pipeserv: can't sync with poke thread, ec=%lu\n", L4_ErrorCode());
		abort();
	}
}


int main(void)
{
	static const struct pipeserv_impl_vtable vt = {
		.write = &pipe_write, .read = &pipe_read,
		.close = &pipe_close, .pipe = &pipe_pipe,
		.set_flags = &pipe_set_flags,
		.set_notify = &pipe_set_notify,
		.get_status = &pipe_get_status,
	};

	main_tid = L4_MyLocalId();
	my_pid = getpid();
	spawn_poke_thrd();
	htable_init(&client_ht, &rehash_pipeclient, NULL);
	htable_init(&handle_ht, &rehash_pipehandle, NULL);

	for(;;) {
		L4_Word_t status = _muidl_pipeserv_impl_dispatch(&vt);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)
			&& selftest_handling(status))
		{
			/* oof */
		} else if(L4_IsLocalId(muidl_get_sender())
			&& L4_Label(muidl_get_tag()) == 0xbaab)
		{
			/* queue-consumption stimulus, i.e. exit was signaled and some I/O
			 * notifications may fire.
			 */
			consume_lc_queue();
		} else {
			printf("pipeserv: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 1 }.raw);
			L4_LoadMR(1, ENOSYS);
			L4_Reply(muidl_get_sender());
		}
	}
}
