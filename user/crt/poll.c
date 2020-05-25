
/* select(2) and poll(2).
 * TODO: add poll(2).
 */

#include <errno.h>
#include <poll.h>
#include <sys/select.h>

#undef FD_CLR
#undef FD_ISSET
#undef FD_SET


int select(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout)
{
	errno = ENOSYS;
	return -1;
}


int pselect(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	const struct timespec *timeout, const sigset_t *sigmask)
{
	errno = ENOSYS;
	return -1;
}


void FD_CLR(int fd, fd_set *set)
{
}


int FD_ISSET(int fd, fd_set *set)
{
	return 0;
}


void FD_SET(int fd, fd_set *set)
{
}


void FD_ZERO(fd_set *set)
{
}


int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	errno = -ENOSYS;
	return -1;
}
