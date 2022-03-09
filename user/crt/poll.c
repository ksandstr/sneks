/* select(2) and poll(2). */

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/epoll.h>

#include <ccan/minmax/minmax.h>


/* notice how bullshit this is? that's select(2). */
int select(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout)
{
	if(nfds < 0) {
		errno = EINVAL;
		return -1;
	}

	static fd_set dummy = { };
	if(readfds == NULL) readfds = &dummy;
	if(writefds == NULL) writefds = &dummy;
	if(exceptfds == NULL) exceptfds = &dummy;

	int epfd = epoll_create1(0);
	if(epfd < 0) return -1;

	const int limb_bits = 8 * sizeof readfds->fds_bits[0];
	int n_evs = 0, limbs = (nfds + limb_bits - 1) / limb_bits;
	for(int j=0; j < limbs; j++) {
		int i = ffsl(readfds->fds_bits[j] | writefds->fds_bits[j] | exceptfds->fds_bits[j]);
		if(i == 0) continue;
		i += j-- * limb_bits - 1;

		int evs = 0;
		if(FD_ISSET(i, readfds)) {
			assert(readfds != &dummy);
			evs |= EPOLLIN;
			FD_CLR(i, readfds);
		}
		if(FD_ISSET(i, writefds)) {
			assert(writefds != &dummy);
			evs |= EPOLLOUT;
			FD_CLR(i, writefds);
		}
		if(FD_ISSET(i, exceptfds)) {
			assert(exceptfds != &dummy);
			evs |= EPOLLERR;	/* others? */
			FD_CLR(i, exceptfds);
		}
		assert(evs != 0);
		evs |= EPOLLEXCLUSIVE;
		int n = epoll_ctl(epfd, EPOLL_CTL_ADD, i,
			&(struct epoll_event){ .events = evs, .data.fd = i });
		if(n < 0) {
			int err = errno;
			close(epfd);
			errno = err;
			return -1;
		}
		n_evs++;
	}

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
		if(evs[i].events & EPOLLIN) {
			assert(readfds != &dummy);
			FD_SET(evs[i].data.fd, readfds);
		}
		if(evs[i].events & EPOLLOUT) {
			assert(writefds != &dummy);
			FD_SET(evs[i].data.fd, writefds);
		}
		if(evs[i].events & EPOLLERR) {
			assert(exceptfds != &dummy);
			FD_SET(evs[i].data.fd, exceptfds);
		}
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
				.events = EPOLLEXCLUSIVE | (fds[i].events & pass),
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
	return got;
}
