
/* <sys/epoll.h>, intended compatible with (some portion of) the same in
 * Linux.
 */

#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include <sys/signal.h>


#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* TODO: move these into <bits/epoll.h> so that root/main.c can consume them
 * for bootcon purposes.
 */
#define EPOLLIN 0x001
#define EPOLLPRI 0x002
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDHUP 0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)


typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};


extern int epoll_create(int size);
extern int epoll_create1(int flags);

extern int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern int epoll_wait(int epfd,
	struct epoll_event *events, int maxevents,
	int timeout);
extern int epoll_pwait(int epfd,
	struct epoll_event *events, int maxevents,
	int timeout, const sigset_t *sigmask);


#endif
