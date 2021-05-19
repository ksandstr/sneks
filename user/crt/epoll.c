
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/list/list.h>
#include <ccan/htable/htable.h>
#include <ccan/array_size/array_size.h>
#include <ccan/minmax/minmax.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/syscall.h>

#include <sneks/hash.h>
#include <sneks/process.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/poll-defs.h>

#include "private.h"


#define EVBUFSZ (4096 / (sizeof(L4_Word_t) * 2))


/* hashed by handle and spid. */
struct interest
{
	struct epoll_event ev;
	unsigned short spid, fd;
	L4_Word_t handle;
};


struct epoll
{
	struct list_node wait_link, all_link;
	int last_sync, n_level;
	L4_ThreadId_t waiter;
	struct htable fds;	/* of <struct interest *> */
};


/* "event queue". allocated on poll_notify_fn()'s stack, which is at least 16k
 * deep so it's fine to munch. not actually a queue.
 */
struct evq
{
	int n_evs, n_heads;
	struct {
		unsigned short pid, count;
	} heads[EVBUFSZ / 2];
	union {
		struct { L4_Word_t mask, handle; };
		L4_Word_t w[2];
	} evbuf[EVBUFSZ];
};


/* see evq_first(), evq_next() */
struct evq_iter {
	int from;	/* current q->heads[h].pid */
	int h, e_base, j, count;
	int live_heads, live_evs;
};


static void epoll_close(L4_Word_t handle);
static bool evq_first(
	L4_Word_t **maskpp, L4_Word_t *handle_p,
	struct evq *q, struct evq_iter *it);
static bool evq_next(
	L4_Word_t **maskpp, L4_Word_t *handle_p,
	struct evq *q, struct evq_iter *it);


static L4_ThreadId_t poll_tid = { .raw = 0 }, poll_ltid;
static struct list_head waiting_epolls = LIST_HEAD_INIT(waiting_epolls),
	all_epolls = LIST_HEAD_INIT(all_epolls);
static _Atomic int sigio_sync_count = 0;


static size_t rehash_interest(const void *ptr, void *priv) {
	const struct interest *in = ptr;
	return int_hash(in->handle ^ (L4_Word_t)in->spid << 16);
}


static int cmp_interest_by_spid_handle_asc(const void *a, const void *b) {
	const struct interest *aa = a, *bb = b;
	int c = (int)aa->spid - (int)bb->spid;
	if(c == 0) c = (int)aa->handle - (int)bb->handle;
	return c;
}


static int cmp_interest_ptr_by_spid_handle_asc(const void *a, const void *b) {
	const struct interest *const *aa = a, *const *bb = b;
	int c = (int)(*aa)->spid - (int)(*bb)->spid;
	if(c == 0) c = (int)(*aa)->handle - (int)(*bb)->handle;
	return c;
}


static void sigio_handler(int signo) {
	atomic_fetch_add_explicit(&sigio_sync_count, 1, memory_order_relaxed);
}


static int poll_levels(
	struct epoll *ep,
	struct epoll_event *events, int maxevents,
	struct interest **ls, int n_ls)
{
	/* probe each span in turn. */
	uint16_t notif[SNEKS_POLL_STBUF_SIZE];
	int hbuf[SNEKS_POLL_STBUF_SIZE], got = 0, start = 0;
	while(got < maxevents && start < n_ls) {
		int len = 0, spid = ls[start]->spid;
		while(start + len < n_ls && spid == ls[start + len]->spid
			&& len < ARRAY_SIZE(hbuf))
		{
			struct interest *i = ls[start + len];
			hbuf[len] = i->handle; notif[len] = i->ev.events & 0xffff;
			len++;
		}
		struct fd_bits *bits = __fdbits(ls[start]->fd);
		assert(bits != NULL);
		int n = __io_get_status(bits->server, hbuf, len, notif, len,
			hbuf, &(unsigned){ SNEKS_POLL_STBUF_SIZE });
		if(n != 0) {
			/* FIXME: do something else? */
			fprintf(stderr, "%s: Poll::get_status failed on spid=%d: n=%d\n",
				__func__, spid, n);
			abort();
		}
		for(int i=0; i < len && got < maxevents; i++) {
			if(hbuf[i] == ~0ul) continue;
			struct interest *fd = ls[start + i];
			L4_Word_t hit = (fd->ev.events | EPOLLHUP) & hbuf[i];
			if(hit > 0) {
				events[got++] = (struct epoll_event){
					.events = hit, .data = fd->ev.data,
				};
			}
		}
		start += len;
	}

	return got;
}


/* sadly, gcc doesn't separate this out from evq_next() on its own. so we have
 * this, the rare half; and evq_next(), the common one.
 */
static bool _evq_next_head(
	L4_Word_t **maskpp, L4_Word_t *handle_p,
	struct evq *q, struct evq_iter *it)
{
	if(it->live_evs == 0) {
		it->live_heads++;
		q->heads[it->h].pid = 0;
	}
	it->live_evs = 0;
	it->j = -1;
	while(it->e_base += q->heads[it->h].count, ++it->h < q->n_heads) {
		it->count = q->heads[it->h].count;
		it->from = q->heads[it->h].pid;
		if(it->from != 0) break;
	}
	if(it->h < q->n_heads) {
		return evq_next(maskpp, handle_p, q, it);
	} else {
		if(it->live_heads == 0) {
			q->n_heads = 0;
			q->n_evs = 0;
		}
		/* TODO: compress @q if not empty? */
		return false;
	}
}


static inline bool evq_next(
	L4_Word_t **maskpp, L4_Word_t *handle_p,
	struct evq *q, struct evq_iter *it)
{
	if(likely(++it->j < it->count)) {
		*maskpp = &q->evbuf[it->e_base + it->j].mask;
		*handle_p = q->evbuf[it->e_base + it->j].handle;
		if(*maskpp != 0) it->live_evs++;
		return true;
	} else {
		return _evq_next_head(maskpp, handle_p, q, it);
	}
}


static bool evq_first(
	L4_Word_t **maskpp, L4_Word_t *handle_p,
	struct evq *q, struct evq_iter *it)
{
	it->e_base = 0; it->live_heads = 0; it->live_evs = 0;
	for(int i=0; i < q->n_heads; it->e_base += q->heads[i++].count) {
		assert(q->heads[i].count > 0);
		if(q->heads[i].pid != 0) {
			it->h = i;
			it->from = q->heads[i].pid;
			it->count = q->heads[i].count;
			it->j = -1;
			return evq_next(maskpp, handle_p, q, it);
		}
	}
	return false;
}


/* merge as many signals from @q as match the interest list in @ep, and return
 * events for the first @maxevents thereof. if afterward there's room and @ep
 * has level-triggered interests that weren't matched from @q, poll them and
 * return as many as fits.
 */
static int epoll_consume(
	struct epoll *ep,
	struct epoll_event *events, int maxevents,
	struct evq *q)
{
	struct interest *levs_hit[ep->n_level];	/* haha stack go brrrrr */
	int got = 0, n_levs_hit = 0;
	struct evq_iter it;
	L4_Word_t *mask, handle;
	for(bool have = evq_first(&mask, &handle, q, &it);
		have && got < maxevents;
		have = evq_next(&mask, &handle, q, &it))
	{
		if(*mask == 0) continue;
		struct interest key = { .handle = handle, .spid = it.from };
		size_t hash = rehash_interest(&key, NULL);
		struct htable_iter it;
		for(struct interest *cand = htable_firstval(&ep->fds, &it, hash);
			cand != NULL && got < maxevents;
			cand = htable_nextval(&ep->fds, &it, hash))
		{
			if(cand->spid != key.spid || cand->handle != key.handle) continue;
			uint32_t hit = cand->ev.events & *mask;
			if(hit == 0) continue;
			events[got++] = (struct epoll_event){
				.events = hit, .data = cand->ev.data,
			};
			*mask &= ~hit;
			/* TODO: support EPOLLONESHOT */
			if(~cand->ev.events & EPOLLET) {
				assert(n_levs_hit < ep->n_level);
				levs_hit[n_levs_hit++] = cand;
			}
		}
	}

	if(got < maxevents && n_levs_hit < ep->n_level) {
		struct interest *ls[ep->n_level];
		qsort(levs_hit, n_levs_hit, sizeof levs_hit[0],
			&cmp_interest_ptr_by_spid_handle_asc);
		/* collect level-triggered interests which aren't in levs_hit[]. */
		int n_ls = 0;
		struct htable_iter it;
		for(struct interest *cand = htable_first(&ep->fds, &it);
			cand != NULL;
			cand = htable_next(&ep->fds, &it))
		{
			if(cand->ev.events & EPOLLET) continue;
			struct interest *hit = bsearch(&cand, levs_hit, n_levs_hit,
				sizeof levs_hit[0], &cmp_interest_ptr_by_spid_handle_asc);
			if(hit != NULL) continue;
			assert(n_ls < ep->n_level);
			ls[n_ls++] = cand;
		}
		if(n_ls > 0) {
			/* sort by server, poll, and generate events. */
			qsort(ls, n_ls, sizeof ls[0], &cmp_interest_ptr_by_spid_handle_asc);
			int n = poll_levels(ep, &events[got], maxevents - got, ls, n_ls);
			if(n > 0) got += n;
		}
	}

	return got;
}


/* poll in @serv for @handles, and add events produced to @q. if there's no
 * room in @q, add as many events as possible and then bump the SIGIO value so
 * that resync restarts with (hopefully) fewer items in the queue.
 */
static void poll_handles(
	struct evq *q,
	L4_ThreadId_t serv, int spid,
	const int *handles, int n_handles)
{
	if(q->n_heads == ARRAY_SIZE(q->heads) || q->n_evs == EVBUFSZ) {
		/* no room; resync. */
resync:
		atomic_fetch_add(&sigio_sync_count, 1);
		return;
	}

	assert(n_handles > 0 && n_handles <= SNEKS_POLL_STBUF_SIZE);
	int st[SNEKS_POLL_STBUF_SIZE];
	unsigned n_st = ARRAY_SIZE(st);
	int n = __io_get_status(serv, handles, n_handles, NULL, 0, st, &n_st);
	if(n != 0) {
		fprintf(stderr, "%s: get_status for serv=%lu:%lu failed, n=%d\n",
			__func__, L4_ThreadNo(serv), L4_Version(serv), n);
		goto resync;
	}

	int new = 0;
	L4_Word_t *evbuf = q->evbuf[q->n_evs].w;
	for(int i=0; i < n_st && new < EVBUFSZ - q->n_evs; i++) {
		if(st[i] == 0 || ~st[i] == 0) continue;
		evbuf[new * 2] = st[i];
		evbuf[new * 2 + 1] = handles[i];
		new++;
	}
	q->heads[q->n_heads].pid = spid;
	q->heads[q->n_heads].count = new;
	q->n_heads++;
	q->n_evs += new;
}


/* like epoll_consume(), but this'll gather edge-triggered interest records
 * that must be re-queried explicitly when SIGIO is raised; i.e. those that
 * aren't matched by @q's contents.
 */
static int epoll_resync_consume(
	struct epoll *ep,
	struct epoll_event *events, int maxevents,
	struct evq *q)
{
	struct interest *fds = malloc(sizeof *fds * ep->fds.elems);
	if(fds == NULL) {
		fprintf(stderr, "%s: malloc failed for %d fds\n", __func__,
			(int)ep->fds.elems);
		abort();
	}
	int n_fds = 0;
	struct htable_iter ith;
	for(const struct interest *in = htable_first(&ep->fds, &ith);
		in != NULL;
		in = htable_next(&ep->fds, &ith))
	{
		assert(n_fds < ep->fds.elems);
		fds[n_fds] = *in;
		n_fds++;
	}
	qsort(fds, n_fds, sizeof *fds, &cmp_interest_by_spid_handle_asc);

	/* then like epoll_consume(), but with bsearch() over the sorted array of
	 * interest rather than htable queries, and marking each hit fd as already
	 * seen.
	 *
	 * NB: this'll miss events for fd=0xffff, because that's used as a "dead"
	 * value and therefore skipped.
	 */
	int got = 0;
	struct evq_iter it;
	L4_Word_t *mask, handle;
	for(bool have = evq_first(&mask, &handle, q, &it);
		have; have = evq_next(&mask, &handle, q, &it))
	{
		if(*mask == 0) continue;
		struct interest key = { .handle = handle, .spid = it.from },
			*ex = bsearch(&key, fds, n_fds, sizeof *fds,
				&cmp_interest_by_spid_handle_asc);
		assert(ex == NULL
			|| (ex->handle == key.handle && ex->spid == key.spid));
		if(ex == NULL || ex->fd == 0xffff) continue;
		uint32_t hit = ex->ev.events & *mask;
		if(!hit) continue;
		if(got < maxevents) {
			events[got++] = (struct epoll_event){
				.events = hit, .data = ex->ev.data,
			};
			*mask &= ~hit;
			/* TODO: support EPOLLONESHOT */
		}
		if(hit == ex->ev.events) ex->fd = 0xffff;
	}

	int bufpid = -1, buflen = 0, hbuf[SNEKS_POLL_STBUF_SIZE];
	L4_ThreadId_t bufserv = L4_nilthread;
	for(int i=0; i < n_fds; i++) {
		if(fds[i].fd == 0xffff) continue;
		struct fd_bits *bits = __fdbits(fds[i].fd);
		if(bits == NULL) continue;
		if(bufpid != fds[i].spid || buflen == ARRAY_SIZE(hbuf)) {
			if(buflen > 0) {
				assert(!L4_IsNilThread(bufserv));
				assert(bufpid > 0);
				poll_handles(q, bufserv, bufpid, hbuf, buflen);
				buflen = 0;
			}
			bufpid = fds[i].spid;
			bufserv = bits->server;
			assert(bufpid == pidof_NP(bufserv));
		}
		hbuf[buflen++] = fds[i].handle;
	}
	if(buflen > 0) {
		poll_handles(q, bufserv, bufpid, hbuf, buflen);
	}

	if(got < maxevents) {
		got += epoll_consume(ep, &events[got], maxevents - got, q);
	}

	free(fds);
	return got;
}


static void epoll_wake(struct evq *q, int n_evs, int from_pid)
{
	struct interest key = { .spid = from_pid };
	for(int i = q->n_evs;
		i < q->n_evs + n_evs && !list_empty(&waiting_epolls);
		i++)
	{
		L4_Word_t mask = q->evbuf[i].mask;
		if(mask == 0) continue;
		key.handle = q->evbuf[i].handle;
		size_t hash = rehash_interest(&key, NULL);
		struct epoll *ep, *next;
		list_for_each_safe(&waiting_epolls, ep, next, wait_link) {
			assert(!L4_IsNilThread(ep->waiter));
			bool hit = false;
			struct htable_iter it;
			for(struct interest *cand = htable_firstval(&ep->fds, &it, hash);
				cand != NULL && !hit;
				cand = htable_nextval(&ep->fds, &it, hash))
			{
				if(cand->spid != from_pid || cand->handle != key.handle) continue;
				if(cand->ev.events & mask) hit = true;
			}
			if(hit) {
				/* TODO: as an optimization, we could return a single event
				 * and turn those bits in the event's mask off. until tests
				 * are stronger well rely on epoll_consume() for authoritative
				 * results though.
				 */
				L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
				L4_LoadMR(1, EAGAIN);
				L4_MsgTag_t tag = L4_Reply(ep->waiter);
				if(L4_IpcSucceeded(tag) || L4_ErrorCode() != 2) {
					ep->waiter = L4_nilthread;
					list_del_from(&waiting_epolls, &ep->wait_link);
				}
			}
		}
	}
}


static noreturn void poll_notify_fn(void *unused)
{
	struct evq q;
	q.n_evs = 0; q.n_heads = 0;
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Wait(&sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				fprintf(stderr, "%s: IPC failed, ec=%lu\n",
					__func__, L4_ErrorCode());
				break;
			}

			if(L4_IsLocalId(sender) && L4_Label(tag) == 0xe803) {
				/* Sneks::IO */
				L4_Word_t op, handle;
				L4_StoreMR(1, &op);
				if(op != 0xabcd) {
					/* not close? */
					L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
					L4_LoadMR(1, ENOSYS);
				} else {
					/* close */
					L4_StoreMR(2, &handle);
					epoll_close(handle);
					L4_LoadMR(0, 0);
				}
			} else if(L4_IsLocalId(sender) && tag.X.u == 3) {
				/* epoll_wait */
				L4_Word_t handle; L4_StoreMR(1, &handle);
				L4_Word_t eventsptr; L4_StoreMR(2, &eventsptr);
				L4_Word_t maxevents; L4_StoreMR(3, &maxevents);
				bool nohang = !!(maxevents & 0x10000);
				maxevents &= 0xffff;
				struct epoll *ep = (struct epoll *)handle;
				if(L4_SameThreads(ep->waiter, sender)) {
					/* timeout idempotence */
					list_del_from(&waiting_epolls, &ep->wait_link);
					ep->waiter = L4_nilthread;
				}
				struct epoll_event *events = (struct epoll_event *)eventsptr;
				int cur_sync = atomic_load_explicit(&sigio_sync_count,
					memory_order_relaxed);
				int got;
				if(ep->last_sync < cur_sync) {
					got = epoll_resync_consume(ep, events, maxevents, &q);
					ep->last_sync = cur_sync;
					atomic_signal_fence(memory_order_release);
				} else {
					got = epoll_consume(ep, events, maxevents, &q);
				}
				if(got == 0 && !nohang) {
					ep->waiter = sender;
					list_add_tail(&waiting_epolls, &ep->wait_link);
					break;
				}
				L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
				L4_LoadMR(1, got);
			} else if((~tag.X.u & 1)
				&& (L4_IsLocalId(sender) || L4_Label(tag) >= SNEKS_MIN_SYSID)
				&& L4_Label(tag) == pidof_NP(sender))
			{
				/* notifications from either the indicated systask, or locally
				 * from epoll_ctl().
				 */
				int count = tag.X.u / 2;
				if(q.n_heads == ARRAY_SIZE(q.heads)
					|| count > ARRAY_SIZE(q.evbuf) - q.n_evs)
				{
					/* overflow, TODO: force sync. */
					fprintf(stderr, "%s: event %s overflow\n", __func__,
						q.n_heads < ARRAY_SIZE(q.heads) ? "buffer" : "head");
					break;
				}
				L4_StoreMRs(1, tag.X.u, q.evbuf[q.n_evs].w);
				epoll_wake(&q, count, L4_Label(tag));
				/* NOTE: should we coalesce events so that if re-arming
				 * I/O happens not in response to epoll_wait(), spurious
				 * edge signals don't appear? this seems like a lot of
				 * work for a tiny bit of userspace piety. (meh, it can be
				 * made stricter later if need be.)
				 */
				q.heads[q.n_heads].pid = L4_Label(tag);
				q.heads[q.n_heads].count = count;
				q.n_heads++;
				q.n_evs += count;
				break;
			} else {
				L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
				L4_LoadMR(1, ENOSYS);
			}
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_ReplyWait(sender, &sender);
		}
	}
}


int epoll_create(int size) {
	return epoll_create1(0);
}


int epoll_create1(int flags)
{
	if(unlikely(L4_IsNilThread(poll_tid))) {
		int n = __crt_thread_create(&poll_tid, &poll_notify_fn, NULL);
		if(n != 0) {
			fprintf(stderr, "%s: can't create poll thread, n=%d\n",
				__func__, n);
			return NTOERR(n);
		}
		assert(!L4_IsNilThread(poll_tid));
		poll_ltid = L4_LocalIdOf(poll_tid);

		/* commandeer SIGIO.
		 *
		 * TODO: use SA_SIGINFO and its siginfo_t to filter sigio response.
		 * for now, since sneks doesn't deliver that, we'll stick with "all
		 * the notifications were missed".
		 */
		struct sigaction act = { .sa_handler = &sigio_handler };
		n = sigaction(SIGIO, &act, NULL);
		if(n < 0) {
			fprintf(stderr, "%s: can't commandeer SIGIO, errno=%d\n",
				__func__, errno);
			abort();
		}

		/* synchronize. */
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_LoadMR(0, 0);
		L4_Call(poll_tid);
	}

	struct epoll *ep = malloc(sizeof *ep);
	if(ep == NULL) {
		errno = ENOMEM;
		return -1;
	}
	*ep = (struct epoll){ .last_sync = atomic_load(&sigio_sync_count) };
	htable_init_sized(&ep->fds, &rehash_interest, NULL, 32);

	/* TODO: transfer EPFL_CLOEXEC (or some such) in @flags to FD_CLOEXEC */
	int fd = __create_fd(-1, poll_tid, (intptr_t)ep, 0);
	if(fd >= 0) {
		list_add_tail(&all_epolls, &ep->all_link);
		return fd;
	} else {
		htable_clear(&ep->fds);
		free(ep);
		errno = -fd;
		return -1;
	}
}


/* this reaches into all structs epoll in the program. them's the breaks. */
static int refresh_notify(size_t hash, L4_ThreadId_t server, L4_Word_t handle)
{
	uint32_t newmask = 0;
	int spid = pidof_NP(server);
	struct epoll *ep;
	list_for_each(&all_epolls, ep, all_link) {
		struct htable_iter it;
		for(const struct interest *reg = htable_firstval(&ep->fds, &it, hash);
			reg != NULL; reg = htable_nextval(&ep->fds, &it, hash))
		{
			if(reg->handle != handle || reg->spid != spid) continue;
			newmask |= reg->ev.events;
		}
	}
	int exmask;
	int n = __io_set_notify(server, &exmask, handle, newmask, poll_tid.raw);
	return NTOERR(n, exmask);
}


/* called from sync IPC w/ main program, which therefore isn't and won't be in
 * malloc().
 */
static void epoll_close(L4_Word_t handle)
{
	struct epoll *ep = (struct epoll *)handle;
	if(!L4_IsNilThread(ep->waiter)) {
		list_del_from(&waiting_epolls, &ep->wait_link);
	}
	list_del_from(&all_epolls, &ep->all_link);
	struct htable_iter it;
	for(struct interest *i = htable_first(&ep->fds, &it);
		i != NULL; i = htable_next(&ep->fds, &it))
	{
		struct fd_bits *bits = __fdbits(i->fd);
		if(bits != NULL) {
			assert(bits->handle == i->handle);
			refresh_notify(rehash_interest(i, NULL),
				bits->server, i->handle);
		}
		free(i);
	}
	htable_clear(&ep->fds);
	free(ep);
}


static size_t direct_rehash(const void *key, void *priv) {
	return (size_t)key ^ 0xf0adf0ad;
}


static bool direct_eq_fn(const void *cand, void *key) {
	return cand == key;
}


/* returns false on the first call for each value of @spid in the current
 * process, and true thereafter.
 */
static bool check_notify(int spid)
{
	static int last_pid = -1;
	static struct htable ht = HTABLE_INITIALIZER(ht, &direct_rehash, NULL);
	if(unlikely(last_pid != getpid())) {
		htable_clear(&ht);
		last_pid = getpid();
	} else if(likely(htable_get(&ht, direct_rehash((void *)spid, NULL),
		&direct_eq_fn, (void *)spid) != NULL))
	{
		return true;
	}

	bool ok = htable_add(&ht, direct_rehash((void *)spid, NULL),
		(void *)spid);
	assert(ok);
	return false;
}


int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	struct fd_bits *epbits = __fdbits(epfd);
	if(epbits == NULL) goto Ebadf;
	L4_ThreadId_t serv = epbits->server;

	assert(!L4_IsNilThread(L4_LocalIdOf(poll_tid)));
	if(unlikely(!L4_SameThreads(serv, poll_tid))) goto Einval;
	struct epoll *ep = (struct epoll *)epbits->handle;

	struct fd_bits *fdbits = __fdbits(fd);
	if(fdbits == NULL) goto Ebadf;
	struct interest key = {
		.spid = pidof_NP(fdbits->server), .handle = fdbits->handle,
	};
	size_t hash = rehash_interest(&key, NULL);
	struct interest *old;
	struct htable_iter it;
	for(old = htable_firstval(&ep->fds, &it, hash);
		old != NULL; old = htable_nextval(&ep->fds, &it, hash))
	{
		if(old->fd == fd) {
			assert(old->spid == key.spid);
			assert(old->handle == key.handle);
			break;
		}
	}
	switch(op) {
		default: goto Einval;

		case EPOLL_CTL_ADD: {
			if(old != NULL) goto Eexist;
			if(event == NULL) goto Einval;
			const int unsupported = EPOLLONESHOT | EPOLLWAKEUP;
			const int required = EPOLLEXCLUSIVE;
			if((event->events & unsupported) || (~event->events & required)) {
				goto Einval;
			}
			struct interest *new = malloc(sizeof *new);
			if(new == NULL) goto Enomem;
			*new = (struct interest){
				.spid = key.spid, .handle = key.handle, .fd = fd,
				.ev.events = event->events | EPOLLHUP,
				.ev.data = event->data,
			};
			bool ok = htable_add(&ep->fds, hash, new);
			if(!ok) { free(new); goto Enomem; }
			if(~new->ev.events & EPOLLET) {
				/* skip mask refresh; it's not necessary to get notifications
				 * for a level-triggered event because status will be queried
				 * anyhow.
				 *
				 * this saves one context switch per fd per call to select(2)
				 * or poll(2).
				 */
				ep->n_level++;
				/* ... except for the first time around in each process, so
				 * that Poll::set_notify.tid is set in each IO server.
				 */
				if(likely(check_notify(key.spid))) return 0;
			}
			int n = refresh_notify(hash, fdbits->server, key.handle);
			if(n < 0) {
				htable_del(&ep->fds, hash, new);
				free(new);
				/* (an IO::set_flags error means "not supported".) */
				goto Eperm;
			} else if(n != 0) {
				/* add these events to queue so they're picked up */
				L4_MsgTag_t tag = (L4_MsgTag_t){ .X.label = key.spid, .X.u = 2 };
				L4_Set_Propagation(&tag);
				L4_Set_VirtualSender(fdbits->server);
				L4_LoadMR(0, tag.raw);
				L4_LoadMR(1, n);
				L4_LoadMR(2, key.handle);
				tag = L4_Send(poll_ltid);
				if(L4_IpcFailed(tag)) {
					fprintf(stderr, "%s [add]: send of existing events failed, ec=%lu\n",
						__func__, L4_ErrorCode());
					/* FIXME: force sync */
					abort();
				}
			}
			return 0;
		}

		case EPOLL_CTL_DEL: {
			if(old == NULL) goto Enoent;
			bool ok = htable_del(&ep->fds, hash, old);
			assert(ok);
			if(~old->ev.events & EPOLLET) {
				assert(ep->n_level > 0);
				ep->n_level--;
			}
			free(old);
			int n = refresh_notify(hash, fdbits->server, key.handle);
			return min(n, 0);
		}

		/* TODO: update oneshot etc. stuff, forbid various other things. this
		 * is generally undertested.
		 */
		case EPOLL_CTL_MOD: {
			if(old == NULL) goto Enoent;
			old->ev = *event; old->ev.events |= EPOLLHUP;
			int n = refresh_notify(hash, fdbits->server, key.handle);
			return min(n, 0);
		}
	}

Einval: errno = EINVAL; return -1;
Enomem: errno = ENOMEM; return -1;
Enoent: errno = ENOENT; return -1;
Eexist: errno = EEXIST; return -1;
Eperm: errno = EPERM; return -1;
Ebadf: errno = EBADF; return -1;
}


int epoll_wait(int epfd,
	struct epoll_event *events, int maxevents,
	int timeout)
{
	struct fd_bits *epbits = __fdbits(epfd);
	if(epbits == NULL) { errno = EBADF; return -1; }
	assert(!L4_IsNilThread(L4_LocalIdOf(poll_tid)));
	if(unlikely(maxevents <= 0 || !L4_SameThreads(epbits->server, poll_tid))) {
		errno = EINVAL;
		return -1;
	}

	L4_Word_t err;
	__permit_recv_interrupt();
	do {
		err = 0;
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 3 }.raw);
		L4_LoadMR(1, epbits->handle);
		L4_LoadMR(2, (L4_Word_t)events);
		L4_LoadMR(3, (maxevents & 0xffff) | (timeout == 0 ? 0x10000 : 0));
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag;
		if(timeout < 0) tag = L4_Lcall(poll_ltid);
		else {
			tag = L4_Call_Timeouts(poll_ltid, L4_Never,
				timeout <= 0 ? L4_Never : L4_TimePeriod(timeout * 1000));
		}
		if(L4_IpcFailed(tag)) {
			int ec = L4_ErrorCode();
			__forbid_recv_interrupt();
			return ec == 3 ? 0 : NTOERR(ec);
		}
		if(L4_Label(tag) == 1) L4_StoreMR(1, &err);
	} while(err == EAGAIN);
	__forbid_recv_interrupt();
	if(err != 0) {
		errno = err;
		return -1;
	} else {
		L4_Word_t count; L4_StoreMR(1, &count);
		return count;
	}
}


int epoll_pwait(int epfd,
	struct epoll_event *events, int maxevents,
	int timeout, const sigset_t *sigmask)
{
	errno = ENOSYS;
	return -1;
}
