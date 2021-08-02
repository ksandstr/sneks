
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H 1

#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>


#define FD_SETSIZE 1024


typedef struct {
	unsigned long fds_bits[FD_SETSIZE / 8 / sizeof(long)];
} fd_set;


#define __FD_LIMB_BITS (sizeof(unsigned long) * 8)

#define FD_CLR(fd, set) do { \
		int __fd = (fd); \
		((set)->fds_bits[__fd / __FD_LIMB_BITS] &= ~(1ul << (__fd % __FD_LIMB_BITS))); \
	} while(0)
#define FD_ISSET(fd, set) ({ \
		int __fd = (fd); \
		!!((set)->fds_bits[__fd / __FD_LIMB_BITS] & (1ul << (__fd % __FD_LIMB_BITS))); \
	})
#define FD_SET(fd, set) do { \
		int __fd = (fd); \
		((set)->fds_bits[__fd / __FD_LIMB_BITS] |= 1ul << (__fd % __FD_LIMB_BITS)); \
	} while(0)
#define FD_ZERO(set) do { \
		fd_set *__fd_zero_set = (set); \
		for(int __fd_zero_i=0; __fd_zero_i < FD_SETSIZE / 8 / sizeof(long); __fd_zero_i++) { \
			__fd_zero_set->fds_bits[__fd_zero_i] = 0; \
		} \
	} while(0)


extern int select(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout);

struct timespec;

extern int pselect(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	const struct timespec *timeout, const sigset_t *sigmask);

#endif
