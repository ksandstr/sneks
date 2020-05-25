
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H 1

#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>


struct timespec;

extern int pselect(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	const struct timespec *timeout, const sigset_t *sigmask);

#endif
