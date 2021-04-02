
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/darray/darray.h>
#include <ccan/compiler/compiler.h>
#include <ccan/minmax/minmax.h>
#include <ccan/htable/htable.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <ukernel/rangealloc.h>
#include <sneks/api/proc-defs.h>
#include <sneks/rollback.h>
#include <sneks/process.h>
#include <sneks/systask.h>
#include <sneks/io.h>

#include "muidl.h"
#include "private.h"


static int (*get_status_callback)(iof_t *) = NULL;


int add_blocker(struct fd *f, L4_ThreadId_t tid, bool writing)
{
	if(~f->owner->flags & CF_NOTIFY) {
		return _nopoll_add_blocker(f, tid, writing);
	}

	/* TODO */
	fprintf(stderr, "%s: blocker_hash not implemented\n", my_name);
	abort();
	return 0;
}


COLD void io_get_status_func(int (*fn)(iof_t *file)) {
	get_status_callback = fn;
}


int io_impl_set_notify(int *exmask_p,
	int fd, int events, L4_Word_t notify_tid_raw)
{
	sync_confirm();

	pid_t caller = pidof_NP(muidl_get_sender());
	struct fd *f = get_fd(caller, fd);
	if(unlikely(f == NULL)) return -EBADF;
	f->flags = (f->flags & ~IOD_EPOLL_MASK) | (events & IOD_EPOLL_MASK);

	if(notify_tid_raw != L4_nilthread.raw) {
		if(!L4_IsGlobalId((L4_ThreadId_t){ .raw = notify_tid_raw })) return -EINVAL;
		struct client *c = f->owner;
		if(~c->flags & CF_NOTIFY) {
			c->flags |= CF_NOTIFY;
			if(!L4_IsNilThread(c->blocker)) {
				add_blocker(f, c->blocker, !!(c->flags & CF_WRITE_BLOCKED));
			}
		}
		c->notify_tid.raw = notify_tid_raw;
	}

	/* NOTE: chrdev.c would cover EPOLLHUP here always, but not in
	 * get_status(). what's up with that? is EPOLLHUP somehow not
	 * level-triggered?
	 */
	*exmask_p = unlikely(get_status_callback == NULL) ? 0
		: (events | EPOLLHUP) & (*get_status_callback)(IOF_T(f->file));
	return 0;
}


void io_impl_get_status(
	const L4_Word_t *handles, unsigned n_handles,
	const uint16_t *notif, unsigned n_notif,
	L4_Word_t *statuses, unsigned *n_statuses_p)
{
	sync_confirm();

	if(get_status_callback == NULL) {
		fprintf(stderr, "%s: get_status callback isn't set?\n", my_name);
		abort();
	}

	n_handles = min_t(unsigned, INT_MAX, n_handles);
	n_notif = min_t(unsigned, INT_MAX, n_notif);
	pid_t caller = pidof_NP(muidl_get_sender());
	for(int i=0; i < n_handles; i++) {
		struct fd *f = get_fd(caller, handles[i]);
		if(unlikely(f == NULL)) {
			statuses[i] = 0;
			continue;
		}
		statuses[i] = (*get_status_callback)(IOF_T(f->file));
		if(likely(i < n_notif)) {
			/* silently ignore bits we don't know about.
			 *
			 * NOTE: since notif[i] is 16 bits wide, this'll always clear the
			 * extended epoll options.
			 */
			f->flags = (f->flags & ~IOD_EPOLL_MASK) | (notif[i] & IOD_EPOLL_MASK);
		}
	}
	*n_statuses_p = n_handles;
}


static void send_poll_event(struct fd *f, int events)
{
	events &= IOD_EPOLL_MASK;
	if((f->flags & events) == 0 || L4_IsNilThread(f->owner->notify_tid)) {
		/* fuck it */
		return;
	}

	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = my_pid, .X.u = 2 }.raw);
	L4_LoadMR(1, events);
	L4_LoadMR(2, ra_ptr2id(fd_ra, f));
	L4_MsgTag_t tag = L4_Reply(f->owner->notify_tid);
	if(L4_IpcFailed(tag)) {
		L4_Word_t ec = L4_ErrorCode();
		if(ec != 2) {
			printf("%s: weird ec=%lu\n", my_name, ec);
			return;
		}
		/* receiver not ready; force sync thru SIGIO. */
		int n = __proc_kill(__uapi_tid, f->owner->pid, SIGIO);
		if(n != 0) {
			printf("%s: Sneks::Proc/kill [SIGIO] to pid=%d failed, n=%d\n",
				my_name, f->owner->pid, n);
			/* ... and do nothing.
			 * TODO: do something?
			 */
		}
	}
}


static void maybe_unblock(L4_ThreadId_t *tid, int mask, bool writing)
{
	if((writing && (mask & EPOLLOUT))
		|| (!writing && (mask & (EPOLLIN | EPOLLHUP))))
	{
		L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
		L4_LoadMR(1, EAGAIN);
		L4_MsgTag_t tag = L4_Reply(*tid);
		if(L4_IpcSucceeded(tag)) *tid = L4_nilthread;
	}
}


void io_notify(iof_t *iof, int epoll_mask)
{
	struct io_file *file = IO_FILE(iof);
	struct fd **fd_it;
	darray_foreach(fd_it, file->handles) {
		struct fd *f = *fd_it;

		/* epoll notifications to actual file handles. */
		send_poll_event(f, epoll_mask);

		/* blocker wakeups. */
		if((~f->owner->flags & CF_NOTIFY) && !L4_IsNilThread(f->owner->blocker)) {
			maybe_unblock(&f->owner->blocker, epoll_mask,
				!!(f->owner->flags & CF_WRITE_BLOCKED));
		} else if(f->owner->flags & CF_NOTIFY) {
			/* TODO: iterate blocker_hash, send unblocks */
		}
	}
}
