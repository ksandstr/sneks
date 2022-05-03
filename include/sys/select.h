#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

#define FD_SETSIZE 1024

typedef struct {
	unsigned long fds_bits[FD_SETSIZE / LONG_BIT];
} fd_set;

#define FD_ZERO(set) do { *(set) = (fd_set){ }; } while(0)
#define FD_ISSET(fd, set) ({ int __fd = (fd); !!((set)->fds_bits[__fd / LONG_BIT] & (1ul << (__fd % LONG_BIT))); })
#define FD_CLR(fd, set) do { \
		int __fd = (fd); \
		((set)->fds_bits[__fd / LONG_BIT] &= ~(1ul << (__fd % LONG_BIT))); \
	} while(0)
#define FD_SET(fd, set) do { \
		int __fd = (fd); \
		((set)->fds_bits[__fd / LONG_BIT] |= 1ul << (__fd % LONG_BIT)); \
	} while(0)

extern int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
extern int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask);

#endif
