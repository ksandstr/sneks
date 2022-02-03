#define SNEKS_IO_IMPL_SOURCE	/* for muidl_raise_no_reply() */
#undef BUILD_SELFTEST

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <limits.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/darray/darray.h>
#include <ccan/htable/htable.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>
#include <ccan/compiler/compiler.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/message.h>

#include <ukernel/rangealloc.h>
#include <sneks/hash.h>
#include <sneks/bitops.h>
#include <sneks/systask.h>
#include <sneks/rollback.h>
#include <sneks/thread.h>
#include <sneks/msg.h>
#include <sneks/api/io-defs.h>
#include <sneks/io.h>

#include "muidl.h"
#include "private.h"


/* missing from CCAN's darray: the order-destroying removal. */
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

#define MAX_FD USHRT_MAX	/* enough for everynyan :3 */
#define NUM_LC_EVENTS 128	/* at most 1024 */


struct fd_transfer {
	int fd;
	pid_t target;
};


struct confirm_data {
	int fd;
	pid_t pid;
	unsigned count;
	off_t offset;
};


/* lifecycle events. these are queued up in lifecycle_handler_fn() and
 * consumed in get_fd() and get_client().
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


static void lifecycle_sync(void);
static bool lifecycle_handler_fn(int, L4_Word_t *, int, void *);

static size_t rehash_client_by_pid(const void *, void *);
static size_t rehash_transfer(const void *, void *);

static void file_dtor(struct io_file *);
static int fd_dtor(struct fd *, bool);
static void client_dtor(struct client *);

int add_blocker(struct fd *, L4_ThreadId_t, bool)
	__attribute__((weak, alias("_nopoll_add_blocker")));


static size_t impl_size;
static int fast_confirm_flags = 0;
static L4_ThreadId_t main_tid;
static thrd_t poke_thrd;
static tss_t confirm_tss;

static struct htable client_hash, transfer_hash;

/* lifecycle processing */
static int lifecycle_msg = -1;	/* sysmsg handle */
/* an ad-hoc SPSC concurrent circular buffer. */
static struct lc_event lce_queue[NUM_LC_EVENTS];
/* its control word. 0..9 = read pos, 10..19 = write pos, 20 = overflow */
static _Atomic uint32_t lce_ctrl;

pid_t my_pid;
struct rangealloc *fd_ra;


static size_t rehash_transfer(const void *ptr, void *priv) {
	const struct fd_transfer *t = ptr;
	return int_hash(t->fd);
}


static size_t rehash_client_by_pid(const void *ptr, void *priv) {
	const struct client *c = ptr;
	return int_hash(c->pid);
}


static bool cmp_client_to_pid(const void *cand, void *key) {
	return *(pid_t *)key == ((struct client *)cand)->pid;
}


#ifdef DEBUG_ME_HARDER
#include <sneks/invariant.h>

#define ra_ptr_valid(ra, ptr) \
	(ra_id2ptr_safe((ra), ra_ptr2id((ra), (ptr))) == (ptr))


static int cmp_fdptr(const void *a, const void *b) {
	return *(struct fd **)a - *(struct fd **)b;
}


static bool transfer_hash_invariants(INVCTX_ARG)
{
	struct htable_iter it;
	for(struct fd_transfer *xfer = htable_first(&transfer_hash, &it);
		xfer != NULL; xfer = htable_next(&transfer_hash, &it))
	{
		inv_push("xfer=%p, ->fd=%d, ->target=%d",
			xfer, xfer->fd, xfer->target);

		struct fd *fd = ra_id2ptr_safe(fd_ra, xfer->fd);
		inv_ok1(fd != NULL);
		inv_ok1(fd->flags & IOD_TRANSFER);

		struct client *c = htable_get(&client_hash, int_hash(xfer->target),
			&cmp_client_to_pid, &xfer->target);
		/* pre-transfer state, or self-dup_to() */
		inv_ok1(fd->owner != c || xfer->target == c->pid);

		inv_pop();
	}
	return true;

inv_fail:
	return false;
}


static bool fd_table_invariants(INVCTX_ARG, struct client *c)
{
	inv_log("c->fd_table_len=%d", c->fd_table_len);

	/* check that target values are valid. */
	for(int i=0; i < c->fd_table_len; i++) {
		if(c->fd_table[i] <= 0) continue;
		inv_push("c->fd_table[%d]=%d", i, c->fd_table[i]);

		inv_ok(c->fd_table_len <= c->fd_table[i]
			|| c->fd_table[c->fd_table[i]] <= 0,
			"indirects exactly once");

		struct fd *fd = ra_id2ptr_safe(fd_ra, c->fd_table[i]);
		inv_ok(fd != NULL, "indirection target exists");
		inv_ok1(fd->owner == c);
		inv_ok1(fd->flags & IOD_SHADOW);

		inv_pop();
	}

	return true;

inv_fail:
	return false;
}


static bool invariants(void)
{
	INV_CTX;

	darray(struct fd *) fdptrs = darray_new();

	/* invariants on extant handles. */
	struct ra_iter rit;
	for(struct fd *fd = ra_first(fd_ra, &rit);
		fd != NULL; fd = ra_next(fd_ra, &rit))
	{
		darray_push(fdptrs, fd);
		inv_push("rangealloc: fd=%p (id=%u)", fd, ra_ptr2id(fd_ra, fd));

		inv_ok1(ra_ptr_valid(fd_ra, fd));
		inv_ok1((fd->flags & ~(IOD_VALID_MASK | IOD_PRIVATE_MASK)) == 0);

		inv_ok1(fd->owner != NULL);
		inv_ok1(fd->owner->handles.size > 0);
		inv_ok1(fd->client_ix < fd->owner->handles.size);
		inv_ok1(fd->owner->handles.item[fd->client_ix] == fd);

		inv_ok1(fd->file != NULL);
		inv_ok1(fd->file->handles.size > 0);
		inv_ok1(fd->file_ix < fd->file->handles.size);
		inv_ok1(fd->file->handles.item[fd->file_ix] == fd);

		int n_xfer = 0;
		if(fd->flags & IOD_TRANSFER) {
			struct htable_iter it;
			size_t hash = int_hash(ra_ptr2id(fd_ra, fd));
			for(struct fd_transfer *xfer = htable_firstval(&transfer_hash, &it, hash);
				xfer != NULL; xfer = htable_nextval(&transfer_hash, &it, hash))
			{
				if(xfer->fd == ra_ptr2id(fd_ra, fd)) n_xfer++;
			}
			inv_log("n_xfer=%d", n_xfer);
		}
		inv_iff1(fd->flags & IOD_TRANSFER, n_xfer == 1);

		inv_pop();
	}

	/* check clients, and discover every file descriptor through them as well
	 * to confirm that the rangealloc iteration found them all.
	 */
	qsort(fdptrs.item, fdptrs.size, sizeof *fdptrs.item, &cmp_fdptr);
	struct htable_iter it;
	for(struct client *c = htable_first(&client_hash, &it);
		c != NULL; c = htable_next(&client_hash, &it))
	{
		inv_push("client: c=%p, ->pid=%d", c, c->pid);

		struct fd **i;
		darray_foreach(i, c->handles) {
			inv_push("handle[%d]: ptr=%p, id=%u",
				i - &c->handles.item[0], *i, ra_ptr2id(fd_ra, *i));

			struct fd **f = bsearch(i, fdptrs.item, fdptrs.size,
				sizeof *fdptrs.item, &cmp_fdptr);
			inv_log("f=%p", f);
			inv_ok(f != NULL, "should be seen in exactly one client");
			inv_ok1((*f)->owner == c);
			darray_remove(fdptrs, f - &fdptrs.item[0]);

			inv_pop();
		}

		inv_ok1(fd_table_invariants(INV_CHILD, c));

		inv_pop();
	}
	inv_ok1(fdptrs.size == 0);

	inv_ok1(transfer_hash_invariants(INV_CHILD));
	darray_free(fdptrs);
	return true;

inv_fail:
	darray_free(fdptrs);
	return false;
}

/* TODO: invariants of the lifecycle queue */

#else
#define invariants() true
#endif


static struct client *get_client(pid_t pid, bool create)
{
	lifecycle_sync();
	size_t hash = int_hash(pid);
	struct client *c = htable_get(&client_hash, hash, &cmp_client_to_pid, &pid);
	if(unlikely(c == NULL) && create) {
		c = malloc(sizeof *c);
		*c = (struct client){ .pid = pid, .handles = darray_new() };
		assert(c->fd_table_len == 0);
		bool ok = htable_add(&client_hash, hash, c);
		if(unlikely(!ok)) { free(c); c = NULL; }

		if(pid <= SNEKS_MAX_PID) {
			static bool first_client = true;
			if(first_client) {
				first_client = false;
				lifecycle_msg = sysmsg_listen(MSGB_PROCESS_LIFECYCLE,
					&lifecycle_handler_fn, NULL);
				if(lifecycle_msg < 0) {
					log_err("can't listen to process lifecycle, n=%d", lifecycle_msg);
					abort();
				}
			}
			sysmsg_add_filter(lifecycle_msg, &(L4_Word_t){ pid }, 1);
		}
	}
	return c;
}


static void client_dtor(struct client *c)
{
	htable_del(&client_hash, int_hash(c->pid), c);
	while(c->handles.size > 0) {
		fd_dtor(c->handles.item[0], true);
	}
	darray_free(c->handles);
	free(c);
	assert(invariants());
}


/* TODO: this function is "a little bit" fucky because it must avoid creating
 * descriptors for which an indirection entry exists in @client. currently
 * this uses a bruteforce approach of storing all "skipped" descriptors in a
 * darray (me caveman, ugh!) and releasing them at the end. becoming
 * accidentally O(n^2) is averted by Hoping Very Hard (tm).
 */
static struct fd *alloc_fd(struct client *client)
{
	darray(struct fd *) skips = darray_new();

	struct fd *ret;
	int desc;
	do {
		ret = ra_alloc(fd_ra, -1);
		if(ret == NULL) break;
		desc = ra_ptr2id(fd_ra, ret);
		if(desc < client->fd_table_len && client->fd_table[desc] > 0) {
			//log_info("skipping desc=%d for client->pid=%d", desc, client->pid);
			darray_push(skips, ret);
		}
	} while(desc < client->fd_table_len && client->fd_table[desc] > 0);

	struct fd **i;
	darray_foreach(i, skips) {
		ra_free(fd_ra, *i);
	}
	darray_free(skips);

	return ret;
}


static int fork_handle(struct fd *fd, struct client *child)
{
	struct fd *copy = alloc_fd(child);
	if(copy == NULL) return -ENOMEM;

	*copy = *fd;
	copy->orig_fd = ra_ptr2id(fd_ra, fd);
	copy->owner = child;
	copy->flags &= ~IOD_TRANSFER;
	copy->flags |= IOD_SHADOW;

	darray_push(child->handles, copy);
	copy->client_ix = child->handles.size - 1;
	darray_push(copy->file->handles, copy);
	copy->file_ix = copy->file->handles.size - 1;

	return ra_ptr2id(fd_ra, copy);
}


/* TODO: generate -ENOMEM, propagate failure as appropriate. the main problem
 * is that since forks are signaled through the sysmsg lifecycle protocol
 * there's not much that we can do if this part of forking fails. presumably
 * we'll just scream into dmesg, leave some FDs invalid, cross fingers, and
 * hope for the best.
 */
static int fork_client(struct client *parent, pid_t child_pid)
{
	assert(invariants());
	size_t hash = int_hash(child_pid);
	if(htable_get(&client_hash, hash, &cmp_client_to_pid, &child_pid) != NULL) {
		log_err("can't fork p=%d because c=%d exists!", parent->pid, child_pid);
		return -EEXIST;
	}

	/* TODO: this is slow, speed it up by having maximum non-shadowed handle
	 * number somewhere.
	 */
	int max_fd = -1;
	struct fd **fd_it;
	darray_foreach(fd_it, parent->handles) {
		if((*fd_it)->flags & IOD_SHADOW) continue;
		max_fd = max_t(int, max_fd, ra_ptr2id(fd_ra, *fd_it));
	}
	for(int i=0; i < parent->fd_table_len; i++) {
		if(parent->fd_table[i] > 0) max_fd = max(max_fd, i);
	}

	struct client *child = calloc(1, sizeof *child + sizeof(int) * (max_fd + 1));
	*child = (struct client){
		.pid = child_pid, .handles = darray_new(),
		.fd_table_len = max_fd + 1,
	};
	bool ok = htable_add(&client_hash, hash, child);
	if(unlikely(!ok)) {
		client_dtor(child);
		return -ENOMEM;
	}

	darray_realloc(child->handles, parent->handles.size);
	/* fork non-shadowed handles. */
	darray_foreach(fd_it, parent->handles) {
		if((*fd_it)->flags & IOD_SHADOW) continue;
		assert(ra_ptr2id(fd_ra, *fd_it) <= max_fd);
		int newfd = fork_handle(*fd_it, child);
		if(newfd < 0) {
			log_crit("non-shadowed fork failure");
			/* TODO: don't shit your pants! */
			abort();
		}

		int pfd = ra_ptr2id(fd_ra, *fd_it);
		assert(pfd < child->fd_table_len);
		assert(child->fd_table[pfd] == 0);
		child->fd_table[pfd] = newfd;
	}

	/* fork shadowed handles from parent's indirection table.
	 *
	 * TODO: this is slow, we should instead iterate over valid indexes of
	 * parent->fd_table[] that were collected in the max_fd loop above.
	 *
	 * alternatively, rangealloc at most 64k <struct io_file> and reference
	 * them by key, and use the other 16 bits to store the non-shadowed
	 * identifier for every handle, and squash this into the handle-iterating
	 * loop.
	 */
	for(size_t i=0; i < min(max_fd + 1, parent->fd_table_len); i++) {
		if(parent->fd_table[i] <= 0) continue;
		struct fd *f = ra_id2ptr(fd_ra, i);
		if(f->owner == NULL) continue;
		assert(~f->flags & IOD_SHADOW);

		int newfd = fork_handle(f, child);
		if(newfd < 0) {
			log_crit("shadowed fork failure");
			/* TODO: fail to soil thyself, barbarian */
			abort();
		}

		assert(i < child->fd_table_len);
		assert(child->fd_table[i] == 0);
		child->fd_table[i] = newfd;
	}
	assert(child->handles.size == parent->handles.size);

	assert(invariants());
	return 0;
}


static struct fd *get_fd_nosync(pid_t pid, int fd)
{
	assert(fd > 0);
	struct fd *f = ra_id2ptr(fd_ra, fd);
	if(likely(f->owner != NULL && f->owner->pid == pid)) {
		/* @fd wasn't inherited thru fork. */
		return unlikely(f->flags & IOD_SHADOW) ? NULL : f;
	} else {
		/* @fd either doesn't exist or belongs to another task. look its copy
		 * up in the fork indirection table.
		 */
		struct client *c = htable_get(&client_hash, int_hash(pid),
			&cmp_client_to_pid, &pid);
		assert(c != NULL);
		if(likely(fd < c->fd_table_len && c->fd_table[fd] > 0)) {
			f = ra_id2ptr(fd_ra, c->fd_table[fd]);
			assert(f == ra_id2ptr_safe(fd_ra, c->fd_table[fd]));
			assert(f->flags & IOD_SHADOW);
			return f;
		} else {
			f = NULL;
		}
		return f;
	}
}


struct fd *get_fd(pid_t pid, int fd) {
	lifecycle_sync();
	return get_fd_nosync(pid, fd);
}


static int fd_dtor(struct fd *fd, bool destroy_file)
{
	int n = 0;
	struct io_file *file = fd->file;

	assert(fd->owner->handles.item[fd->client_ix] == fd);
	darray_remove_fast(fd->owner->handles, fd->client_ix);
	if(fd->client_ix < fd->owner->handles.size) {
		fd->owner->handles.item[fd->client_ix]->client_ix = fd->client_ix;
	}

	assert(file->handles.item[fd->file_ix] == fd);
	darray_remove_fast(file->handles, fd->file_ix);
	if(fd->file_ix < file->handles.size) {
		file->handles.item[fd->file_ix]->file_ix = fd->file_ix;
	}
	if(file->handles.size == 0 && destroy_file) {
		n = (*callbacks.close)(IOF_T(file));
		file_dtor(file);
	}

	if(unlikely(fd->flags & IOD_TRANSFER)) {
		struct htable_iter it;
		int fdnum = ra_ptr2id(fd_ra, fd);
		size_t hash = int_hash(fdnum);
		for(struct fd_transfer *t = htable_firstval(&transfer_hash, &it, hash);
			t != NULL; t = htable_nextval(&transfer_hash, &it, hash))
		{
			if(t->fd == fdnum) {
				htable_delval(&transfer_hash, &it);
				free(t);
			}
		}
	}

	fd->owner = NULL;
	ra_free(fd_ra, fd);

	return n;
}


iof_t *iof_new(int flags)
{
	if(flags & ~IOF_VALID_MASK) return NULL;

	struct io_file *file = malloc(sizeof *file + impl_size);
	if(file == NULL) return NULL;

	*file = (struct io_file){ .handles = darray_new() };
	return IOF_T(file);
}


static void file_dtor(struct io_file *file)
{
	assert(file->handles.size == 0);
	darray_free(file->handles);
	free(file);
}


void iof_undo_new(iof_t *iof)
{
	assert(invariants());
	if(iof == NULL) return;
	struct io_file *file = IO_FILE(iof);
	while(file->handles.size > 0) {
		fd_dtor(file->handles.item[0], false);
	}
	file_dtor(file);
	assert(invariants());
}


int io_add_fd(pid_t pid, iof_t *iof, int flags)
{
	assert(invariants());
	if(flags & ~IOD_VALID_MASK) return -EINVAL;
	struct io_file *file = IO_FILE(iof);

	struct client *c = get_client(pid, true);
	if(c == NULL) return -ENOMEM;
	if(c->handles.size == MAX_FD) return -EMFILE;
	if(file->handles.size == MAX_FD) return -ENFILE;

	struct fd *fd = alloc_fd(c);
	if(fd == NULL) {
		/* TODO: when a client hits (say) 2k main array descriptors, allocate
		 * overflow in a per-client range (darray) identified by fd > MAX_FD.
		 */
		return -ENFILE;
	}
	*fd = (struct fd){
		.file = file, .owner = c, .flags = flags,
		.client_ix = c->handles.size, .file_ix = file->handles.size,
	};
	/* TODO: catch ENOMEM on these two */
	darray_push(c->handles, fd);
	darray_push(file->handles, fd);
	assert(c->handles.item[fd->client_ix] == fd);
	assert(file->handles.item[fd->file_ix] == fd);

	assert(invariants());
	return ra_ptr2id(fd_ra, fd);
}


iof_t *io_get_file(pid_t pid, int fd) {
	assert(invariants());
	struct fd *f;
	if(likely(pid != -1)) f = get_fd(pid, fd);
	else {
		struct ra_iter it;
		f = ra_first(fd_ra, &it);
	}
	return likely(f != NULL) ? IOF_T(f->file) : NULL;
}


int io_impl_set_file_flags(int *old, int fd, int or_mask, int and_mask)
{
	sync_confirm();

	pid_t sender_pid = pidof_NP(muidl_get_sender());
	struct fd *f = get_fd(sender_pid, fd);
	if(f == NULL) return -EBADF;

	const unsigned permitted = IOF_NONBLOCK;
	and_mask |= ~permitted;
	or_mask &= permitted;

	*old = f->file->flags & IOF_VALID_MASK;
	f->file->flags = (*old & and_mask) | or_mask | (*old & ~permitted);

	assert(invariants());
	return 0;
}


int io_impl_set_handle_flags(int *old, int fd, int or_mask, int and_mask)
{
	sync_confirm();

	pid_t sender_pid = pidof_NP(muidl_get_sender());
	struct fd *f = get_fd(sender_pid, fd);
	if(f == NULL) return -EBADF;

	const unsigned permitted = SNEKS_IO_FD_CLOEXEC;
	and_mask |= ~permitted;
	or_mask &= permitted;

	*old = (f->flags & IOD_CLOEXEC) >> 16;
	int new_set = (*old & and_mask) | or_mask | (*old & ~permitted);
	f->flags = (f->flags & ~IOD_VALID_MASK) | ((new_set << 16) & IOD_VALID_MASK);

	assert(invariants());
	return 0;
}


static void io_confirm(L4_Word_t param, struct confirm_data *dat)
{
	/* this should never happen. if it does regardless, scream and fail. */
	if(callbacks.confirm == NULL) {
		log_crit("callbacks.confirm reset between wr_confirm and callback");
		abort();
	}

	struct fd *f = get_fd_nosync(dat->pid, dat->fd);
	/* as a rule, confirms and rollbacks should execute before lifecycle
	 * events are processed so that the clients and descriptors their contexts
	 * reference stay valid. this assert failing indicates that something is
	 * syncing lifecycle stuff at the wrong time.
	 */
	assert(f != NULL);
	(*callbacks.confirm)(IOF_T(f->file), dat->count, dat->offset, !!(param & 1));
	assert(invariants());
}


/* encode the param/priv format for io_confirm(). */
static void wr_confirm(pid_t pid, bool writing,
	int fd, unsigned count, off_t offset)
{
	if(callbacks.confirm == NULL) return;

	struct confirm_data *dat = tss_get(confirm_tss);
	if(unlikely(dat == NULL)) {
		dat = malloc(sizeof *dat);
		if(dat == NULL) {
			log_crit("can't allocate confirm_data");
			abort();
		}
		tss_set(confirm_tss, dat);
	}

	*dat = (struct confirm_data){
		.pid = pidof_NP(muidl_get_sender()),
		.fd = fd, .count = count, .offset = offset,
	};
	set_confirm(&io_confirm, writing ? 1 : 0, dat);
	assert(invariants());
}


int _nopoll_add_blocker(struct fd *f, L4_ThreadId_t tid, bool writing)
{
	assert(~f->owner->flags & CF_NOTIFY);
	if(!L4_IsNilThread(f->owner->blocker)) {
		/* TODO: have an affordance for when a multithreaded userspace has
		 * several threads sleeping on a single character device file, i.e.
		 * move blocker_hash from pollimpl.c back to io.c .
		 */
		log_err("overwriting existing blocker!");
	}
	f->owner->blocker = tid;
	if(writing) f->owner->flags |= CF_WRITE_BLOCKED;
	else f->owner->flags &= ~CF_WRITE_BLOCKED;

	assert(invariants());
	return 0;
}


int io_impl_write(int fd, off_t offset, const uint8_t *buf, unsigned count)
{
	sync_confirm();

	L4_ThreadId_t sender = muidl_get_sender();
	pid_t sender_pid = pidof_NP(sender);
	struct fd *f = get_fd(sender_pid, fd);
	if(f == NULL) return -EBADF;

	/* clamp count to USHRT_MAX because IO::write returns unsigned short. this
	 * is kind of bad and should get fixed at some point.
	 */
	int n = (*callbacks.write)(IOF_T(f->file), buf,
		min_t(unsigned, USHRT_MAX, count), offset);
	if(n == -EWOULDBLOCK && (~f->file->flags & IOF_NONBLOCK)) {
		n = add_blocker(f, sender, true);
		assert(invariants());
		if(n < 0) return n;
		else {
			muidl_raise_no_reply();
			return 0;
		}
	} else {
		if(n > 0) {
			wr_confirm(sender_pid, true, fd, n, offset);
			if(fast_confirm_flags & IO_CONFIRM_WRITE) io_set_fast_confirm();
		}
		assert(invariants());
		return n;
	}
}


int io_impl_read(
	int fd, int length, off_t offset,
	uint8_t *buf, unsigned *buf_len_p)
{
	sync_confirm();

	L4_ThreadId_t sender = muidl_get_sender();
	pid_t sender_pid = pidof_NP(sender);
	struct fd *f = get_fd(sender_pid, fd);
	if(f == NULL) return -EBADF;

	int n = (*callbacks.read)(IOF_T(f->file), buf, max(0, length), offset);
	if(n == -EWOULDBLOCK && (~f->file->flags & IOF_NONBLOCK)) {
		n = add_blocker(f, sender, false);
		assert(invariants());
		if(n < 0) return n;
		else {
			muidl_raise_no_reply();
			return 0;
		}
	} else {
		*buf_len_p = max(n, 0);
		if(*buf_len_p > 0) {
			wr_confirm(sender_pid, false, fd, *buf_len_p, offset);
			if(fast_confirm_flags & IO_CONFIRM_READ) io_set_fast_confirm();
		}
		assert(invariants());
		return min(n, 0);
	}
}


static void late_close_fd(L4_Word_t param, struct fd *f)
{
	assert(f->owner != NULL);

	if(f->flags & IOD_SHADOW) {
		assert(f->orig_fd < f->owner->fd_table_len);
		assert(f->owner->fd_table[f->orig_fd] > 0);
		f->owner->fd_table[f->orig_fd] = 0;
	}

	int n = fd_dtor(f, true);
	if(n != 0) {
		log_err("late close of fd=%p returned n=%d\n", f, n);
		/* not a lot we can do here besides */
	}

	assert(invariants());
}


int io_impl_close(int fd)
{
	sync_confirm();

	if(fd <= 0) return -EBADF;

	pid_t sender_pid = pidof_NP(muidl_get_sender());
	struct fd *f = get_fd(sender_pid, fd);
	if(f == NULL) return -EBADF;

	set_confirm(&late_close_fd, 0, f);
	if(fast_confirm_flags & IO_CONFIRM_CLOSE) io_set_fast_confirm();

	assert(invariants());
	return 0;
}


static int internal_dup(int *newfd_p, int oldfd, pid_t receiver_pid, int flags)
{
	if(flags & ~SNEKS_IO_FD_CLOEXEC) return -EINVAL;

	pid_t sender_pid = pidof_NP(muidl_get_sender());
	struct fd *fd = get_fd(sender_pid, oldfd);
	if(fd == NULL) return -EBADF;

	struct fd_transfer *t = NULL;
	if(receiver_pid >= 0) {
		t = malloc(sizeof *t);
		if(t == NULL) return -ENOMEM;
	}

	int h_flags = fd->flags & ~(IOD_PRIVATE_MASK | IOD_CLOEXEC);
	if(flags & SNEKS_IO_FD_CLOEXEC) h_flags |= IOD_CLOEXEC;
	assert(!!(flags & SNEKS_IO_FD_CLOEXEC) == !!(h_flags & IOD_CLOEXEC));
	int n = io_add_fd(sender_pid, IOF_T(fd->file), h_flags);
	if(n < 0) {
		free(t);
		return n;
	}
	*newfd_p = n;
	struct fd *copy = ra_id2ptr(fd_ra, *newfd_p);
	assert(copy->owner == get_client(sender_pid, false));

	if(receiver_pid >= 0) {
		*t = (struct fd_transfer){ .fd = *newfd_p, .target = receiver_pid };
		bool ok = htable_add(&transfer_hash, int_hash(*newfd_p), t);
		if(unlikely(!ok)) {
			free(t);
			fd_dtor(copy, false);
			return -ENOMEM;
		}
		copy->flags |= IOD_TRANSFER;
	}
	set_rollback(&late_close_fd, *newfd_p, copy);
	assert(invariants());
	return 0;
}


int io_impl_dup(int *newfd_p, int oldfd, int flags) {
	sync_confirm();
	return internal_dup(newfd_p, oldfd, -1, flags);
}


int io_impl_dup_to(int *newfd_p, int oldfd, pid_t receiver_pid) {
	sync_confirm();
	return internal_dup(newfd_p, oldfd, receiver_pid, 0);
}


int io_impl_touch(int newfd)
{
	sync_confirm();

	pid_t sender_pid = pidof_NP(muidl_get_sender());
	struct client *c = get_client(sender_pid, true);
	if(c == NULL) return -ENOMEM;

	struct fd_transfer *found = NULL;
	size_t hash = int_hash(newfd);
	struct htable_iter it;
	for(struct fd_transfer *cand = htable_firstval(&transfer_hash, &it, hash);
		cand != NULL; cand = htable_nextval(&transfer_hash, &it, hash))
	{
		if(cand->fd == newfd && cand->target == sender_pid) {
			found = cand;
			htable_delval(&transfer_hash, &it);
			break;
		}
	}
	if(found == NULL) return -EBADF;

	free(found);
	struct fd *fd = ra_id2ptr(fd_ra, newfd);
	assert(ra_id2ptr_safe(fd_ra, newfd) == fd);
	if(fd->owner == NULL || (~fd->flags & IOD_TRANSFER)) return -EBADF;

	/* remove from previous owner */
	darray_remove_fast(fd->owner->handles, fd->client_ix);
	if(fd->client_ix < fd->owner->handles.size) {
		fd->owner->handles.item[fd->client_ix]->client_ix = fd->client_ix;
	}

	/* transfer to new one */
	fd->owner = c;
	fd->client_ix = fd->owner->handles.size;
	darray_push(fd->owner->handles, fd);
	assert(fd->owner->handles.item[fd->client_ix] == fd);
	fd->flags &= ~IOD_TRANSFER;

	assert(invariants());
	return 0;
}


int io_impl_stat_handle(int fd, struct sneks_io_statbuf *result_ptr)
{
	sync_confirm();
	struct fd *f = get_fd(pidof_NP(muidl_get_sender()), fd);
	return f != NULL ? (*callbacks.stat)(IOF_T(f->file), result_ptr) : -EBADF;
}


/* by default we're never a teletype. those that are will override the vtable
 * entry accordingly.
 */
int io_impl_isatty(int fd) {
	sync_confirm();
	if(get_fd(pidof_NP(muidl_get_sender()), fd) == NULL) return -EBADF;
	return 0;
}


/* thread that converts an "edge" poke into a "level" event. without causing
 * the sysmsg handler to block, this ensures that MPL_EXIT is processed right
 * away so that SIGPIPE and the like go out immediately in response to peer
 * exit.
 *
 * atomicity is guaranteed by microkernel Ipc syscall; the thread is always
 * either waiting to receive, waiting to send, or on its way to send. thus an
 * edge-mode poke will either be merged into an underway send or generate one
 * where there wasn't before.
 *
 * this isn't robust at all against IPC failure but that's fine since it
 * shouldn't occur unless ExchangeRegisters interrupts one of the sleeping
 * sides. if that were to happen, signals may be lost either due to a broken
 * sleeping send or edge pokes coming in between failure handling and the
 * non-send wait in the outer loop. the latter could be solved by running this
 * thread on a priority band above its clients but the former is irreparable;
 * so don't do that, then.
 */
static noreturn int poke_fn(void *param_ptr)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_Never, &sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				log_err("ipc failed, ec=%lu", L4_ErrorCode());
				break;
			}
			L4_ThreadId_t dest; L4_StoreMR(1, &dest.raw);
			L4_Accept(L4_UntypedWordsAcceptor);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.label = L4_Label(tag) }.raw);
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
		log_crit("can't create poke thread, n=%d", n);
		abort();
	}

	/* this puts poke_fn() into the "LreplyWaitLocal" form's receive phase,
	 * which means it's got past the initialization part where it might miss
	 * an early edge poke.
	 */
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xbe7a, .X.u = 1 }.raw);
	L4_LoadMR(1, L4_MyLocalId().raw);
	L4_MsgTag_t tag = L4_Lcall(thrd_to_tid(poke_thrd));
	if(L4_IpcFailed(tag)) {
		log_crit("can't sync with poke thread, ec=%lu", L4_ErrorCode());
		abort();
	}
}


/* drain the lifecycle message queue. */
static void lifecycle_sync(void)
{
	uint32_t ctrl = atomic_load_explicit(&lce_ctrl, memory_order_acquire);
	if(unlikely(ctrl & (1 << 20))) {
		/* TODO: add the "list_children" thing, program image timestamps, and
		 * so forth for manual synchronization when lifecycle events were
		 * lost.
		 */
		log_err("lost lifecycle events!");
		abort();
	}
	if(likely(ctrl >> 10 == (ctrl & 0x3ff))) {
		/* queue is empty. */
		return;
	}

	int first = ctrl >> 10, count = (int)(ctrl & 0x3ff) - first;
	if(count < 0) count += NUM_LC_EVENTS;
	assert(count > 0);
	for(int i=0; i < count; i++) {
		const struct lc_event *cur = &lce_queue[(first + i) % NUM_LC_EVENTS];
		pid_t p = cur->primary;
		/* avoid deadly recursion thru get_client(). */
		struct client *c = htable_get(&client_hash, int_hash(p),
			&cmp_client_to_pid, &p);
		if(c == NULL) {
			/* spurious, which is ok */
			continue;
		}
		switch(cur->tag) {
			case MPL_FORK:
				fork_client(c, cur->child);
				(*callbacks.lifecycle)(c->pid, CLIENT_FORK, cur->child);
				break;
			case MPL_EXEC:
				for(size_t i=0; i < c->handles.size; i++) {
					struct fd *fd = c->handles.item[i];
					if(~fd->flags & IOD_CLOEXEC) continue;
					late_close_fd(0, fd);
					i--;
				}
				(*callbacks.lifecycle)(c->pid, CLIENT_EXEC);
				break;
			case MPL_EXIT:
				client_dtor(c);
				(*callbacks.lifecycle)(c->pid, CLIENT_EXIT);
				sysmsg_rm_filter(lifecycle_msg, &(L4_Word_t){ p }, 1);
				break;
			default:
				log_info("weird lifecycle tag=%d", cur->tag);
		}
	}

	uint32_t newctrl;
	do {
		assert(ctrl >> 10 == first);	/* single consumer */
		newctrl = (first + count) % NUM_LC_EVENTS << 10 | (ctrl & 0x3ff);
	} while(!atomic_compare_exchange_strong(&lce_ctrl, &ctrl, newctrl));
}


/* this adds filters for forked processes immediately and records other events
 * in the event log. if the event log becomes full, filter adds are still
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
			log_info("unexpected lifecycle tag=%#x", ev->tag);
	}

	uint32_t newctrl;
	do {
		assert((ctrl & 0x3ff) == wrpos);	/* single producer */
		newctrl = (ctrl & (0x3ff << 10)) | (wrpos + 1) % NUM_LC_EVENTS;
	} while(!atomic_compare_exchange_strong(&lce_ctrl, &ctrl, newctrl)
		&& (~ctrl & (1 << 20)));

	if(poke) {
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 0xbaab }.raw);
		L4_LoadMR(1, main_tid.raw);
		L4_MsgTag_t tag = L4_Reply(thrd_to_tid(poke_thrd));
		if(L4_IpcFailed(tag) && L4_ErrorCode() != 2) {
			log_err("failed lifecycle poke, ec=%lu", L4_ErrorCode());
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


void io_set_fast_confirm(void)
{
	/* it'd be nice if there were (say) a muidl interface for replying
	 * once and returning to confirm immediately. for now this'll do.
	 */
	L4_ThreadId_t poke_tid = thrd_to_tid(poke_thrd);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xbaab, .X.u = 1 }.raw);
	L4_LoadMR(1, main_tid.raw);
	L4_Reply(poke_tid);
}


COLD void io_fast_confirm_flags(int flags) {
	assert((flags & ~IO_CONFIRM_VALID_MASK) == 0);
	fast_confirm_flags = flags;
}


static void prepend_log_string(struct hook *h,
	void *strptr_ptr, uintptr_t level, const char *my_name)
{
	char **spp = strptr_ptr, *new;
	int len = asprintf(&new, "%s:%s", my_name, *spp);
	if(len > 0) {
		free(*spp);
		*spp = new;
	}
}


int io_run(size_t iof_size, int argc, char *argv[])
{
	impl_size = iof_size;
	/* TODO: configure max# of fds on the command line, or a configuration
	 * call; certainly e.g. nulldevs don't need to reserve a meg of address
	 * space for 64k (i.e. MAX_FD + 1) descriptors.
	 */
	fd_ra = RA_NEW(struct fd, 1 << 16);
	ra_disable_id_0(fd_ra);		/* for client.fd_table, get_fd() */

	main_tid = L4_MyLocalId();
	my_pid = pidof_NP(L4_MyGlobalId());
	spawn_poke_thrd();
	htable_init(&client_hash, &rehash_client_by_pid, NULL);
	htable_init(&transfer_hash, &rehash_transfer, NULL);

	char *my_name;
	asprintf(&my_name, "sys/io[%s:%d]", argc > 0 ? argv[0] : "", my_pid);
	hook_push_back(&log_hook, &prepend_log_string, my_name);

	int n = tss_create(&confirm_tss, &free);
	if(n != thrd_success) {
		log_crit("tss_create() failed");
		return EXIT_FAILURE;
	}

	assert(invariants());

	int rc;
	for(;;) {
		L4_Word_t status = (*callbacks.dispatch)(callbacks.dispatch_priv);
		L4_ThreadId_t sender = muidl_get_sender();
		L4_MsgTag_t tag = muidl_get_tag();
		if(status != 0 && check_rollback(status)) {
			/* rollback triggered by failed reply. no further action is
			 * necessary.
			 */
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)
			&& selftest_handling(status))
		{
			/* selftests were run. */
		} else if(L4_IsLocalId(sender) && L4_Label(tag) == 0xbaab) {
			/* queue-consumption stimulus; some I/O notifications may fire. */
			sync_confirm();		/* a previous reply succeeded. */
			lifecycle_sync();
		} else if(L4_IsLocalId(sender) && (L4_Label(tag) & 0xff00) == 0xff00) {
			/* io_quit() was run */
			rc = L4_Label(tag) & 0xff;
			break;
		} else {
			log_info("dispatch status %#lx (last tag %#lx)", status, tag.raw);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 1 }.raw);
			L4_LoadMR(1, ENOSYS);
			L4_Reply(sender);
		}
		assert(invariants());
	}
	/* TODO: make io_run() reentrant; right now this is only for
	 * Filesystem/shutdown via io_quit().
	 */
	return rc;
}

int io_quit(int rc)
{
	/* utilize the poke thread. */
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xff00 | (rc & 0xff), .X.u = 1 }.raw);
	L4_LoadMR(1, main_tid.raw);
	L4_MsgTag_t tag = L4_Reply(thrd_to_tid(poke_thrd));
	if(L4_IpcSucceeded(tag)) return 0;
	else {
		L4_Word_t ec = L4_ErrorCode();
		if(ec == 2) return -EBUSY;
		else {
			log_info("L4 errorcode=%#lx on reply to poke_thrd", ec);
			return -EIO;
		}
	}
}
