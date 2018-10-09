
#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stdint.h>
#include <sys/types.h>


/* FIXME: get this from somewhere. also __size_t, __intptr_t */
typedef int __pid_t;


extern int getpagesize(void);
extern __pid_t getpid(void);

extern long sysconf(int name);

extern int brk(void *addr);
extern void *sbrk(intptr_t increment);


extern int close(int fd);
extern long read(int fd, void *buf, size_t count);

extern __pid_t fork(void);
extern __pid_t wait(int *status_p);


/* sysconf() names as far as sneks knows of 'em. */
enum {
	_SC_PAGESIZE,
#define _SC_PAGESIZE _SC_PAGESIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
	_SC_NPROCESSORS_ONLN,
};

#endif
