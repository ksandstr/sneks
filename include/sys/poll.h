#ifndef _SYS_POLL_H
#define _SYS_POLL_H

#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

typedef unsigned long int nfds_t;

struct pollfd {
	int fd;
	short events;
	short revents;
};

extern int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
