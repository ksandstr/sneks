/* select(2) and poll(2). */

#include <string.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/epoll.h>

#include <ccan/minmax/minmax.h>


/* notice how bullshit this is? that's select(2).
 *
 * TODO: this could be made more efficient by not specifying EPOLLET in the
 * event mask, but sneks doesn't currently implement it. the difference is
 * worth a context switch per epoll_ctl() call, in favour of batched calls to
 * Poll::get_status per server (just one in the typical case). this is also
 * the difference that a non-epoll based select(2) would have, so it's just as
 * well to get it through epoll.
 */
int select(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout)
{
	if(nfds < 0) {
		errno = EINVAL;
		return -1;
	}

	int epfd = epoll_create1(0);
	if(epfd < 0) return -1;
	int n_evs = 0, limbs = (nfds + __UINTPTR_BITS - 1) / __UINTPTR_BITS;
	for(int j=0; j < limbs; j++) {
		int i = ffsl((readfds != NULL ? readfds->__w[j] : 0)
			| (writefds != NULL ? writefds->__w[j] : 0)
			| (exceptfds != NULL ? exceptfds->__w[j] : 0));
		if(i == 0) continue;
		i += j-- * __UINTPTR_BITS - 1;

		int evs = 0;
		if(readfds != NULL && FD_ISSET(i, readfds)) {
			evs |= EPOLLIN;
			FD_CLR(i, readfds);
		}
		if(writefds != NULL && FD_ISSET(i, writefds)) {
			evs |= EPOLLOUT;
			FD_CLR(i, writefds);
		}
		if(exceptfds != NULL && FD_ISSET(i, exceptfds)) {
			evs |= EPOLLERR;	/* and others? */
			FD_CLR(i, exceptfds);
		}
		assert(evs != 0);
		evs |= EPOLLET | EPOLLEXCLUSIVE;
		struct epoll_event ev = { .events = evs, .data.fd = i };
		int n = epoll_ctl(epfd, EPOLL_CTL_ADD, i, &ev);
		if(n < 0) {
			int err = errno;
			close(epfd);
			errno = err;
			return -1;
		}
		n_evs++;
	}

	fd_set dummy;
	if(readfds == NULL) readfds = &dummy;
	if(writefds == NULL) writefds = &dummy;
	if(exceptfds == NULL) exceptfds = &dummy;

	struct epoll_event evs[n_evs];
	int n = epoll_wait(epfd, evs, n_evs, timeout == NULL ? -1
		: timeout->tv_sec * 1000 + (max_t(int, 0, timeout->tv_usec) + 999) / 1000);
	if(n < 0) {
		int err = errno;
		close(epfd);
		errno = err;
		return -1;
	}
	int count = 0;
	for(int i=0; i < n; i++) {
		if(evs[i].events & EPOLLIN) FD_SET(evs[i].data.fd, readfds);
		if(evs[i].events & EPOLLOUT) FD_SET(evs[i].data.fd, writefds);
		if(evs[i].events & EPOLLERR) FD_SET(evs[i].data.fd, exceptfds);
		if(evs[i].events & (EPOLLIN | EPOLLOUT | EPOLLERR)) count++;
	}

	close(epfd);
	return count;
}


int pselect(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	const struct timespec *timeout, const sigset_t *sigmask)
{
	/* TODO? eventually, alongside other "p" variants. */
	errno = ENOSYS;
	return -1;
}


#undef FD_CLR
void FD_CLR(int fd, fd_set *set) {
	set->__w[fd / __UINTPTR_BITS] &= ~(1ul << (fd % __UINTPTR_BITS));
}


#undef FD_ISSET
int FD_ISSET(int fd, fd_set *set) {
	return !!(set->__w[fd / __UINTPTR_BITS] & (1ul << (fd % __UINTPTR_BITS)));
}


#undef FD_SET
void FD_SET(int fd, fd_set *set) {
	set->__w[fd / __UINTPTR_BITS] |= 1ul << (fd % __UINTPTR_BITS);
}


void FD_ZERO(fd_set *set) {
	memset(set, 0, sizeof *set);
}


/* NOTE: same applies wrt EPOLLET as for select(2). remove it once sneks epoll
 * does level triggering.
 */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	/* (events should be directly convertible.) */
	assert(EPOLLIN == POLLIN);
	assert(EPOLLOUT == POLLOUT);
	assert(EPOLLPRI == POLLPRI);
	assert(EPOLLERR == POLLERR);
	assert(EPOLLHUP == POLLHUP);
	const int pass = EPOLLIN | EPOLLOUT | EPOLLPRI;

	int epfd = epoll_create1(0);
	if(epfd < 0) return -1;

	int n_real = 0;
	for(nfds_t i=0; i < nfds; i++) {
		fds[i].revents = 0;
		if(fds[i].fd < 0) continue;
		int n = epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i].fd,
			&(struct epoll_event){
				.events = EPOLLET | EPOLLEXCLUSIVE | (fds[i].events & pass),
				.data.ptr = &fds[i],
			});
		if(n < 0 && errno == EINVAL) fds[i].revents = POLLNVAL;
		else if(n < 0) {
			int err = errno;
			close(epfd);
			errno = err;
			return -1;
		}
		n_real++;
	}

	struct epoll_event evs[n_real];
	int n = epoll_wait(epfd, evs, n_real, timeout);
	if(n < 0) {
		int err = errno;
		close(epfd);
		errno = err;
		return -1;
	}
	int got = 0;
	for(int i=0; i < n; i++) {
		struct pollfd *p = evs[i].data.ptr;
		assert(p >= fds && p < &fds[nfds]);
		p->revents = evs[i].events & ((p->events & pass) | EPOLLERR | EPOLLHUP);
		if(p->revents != 0) got++;
	}

	close(epfd);
	return n;
}
