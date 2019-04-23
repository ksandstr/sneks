
#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stdint.h>
#include <sys/types.h>


/* FIXME: get this from somewhere. also __size_t, __intptr_t */
typedef int __pid_t;
typedef unsigned int __useconds_t;


extern int getpagesize(void);
extern __pid_t getpid(void);

extern long sysconf(int name);

extern int brk(void *addr);
extern void *sbrk(intptr_t increment);

extern int pause(void);


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


extern unsigned int sleep(unsigned int seconds);
extern int usleep(__useconds_t usec);

extern __uid_t getuid(void);
extern __uid_t geteuid(void);

extern int setuid(__uid_t uid);
extern int seteuid(__uid_t uid);
extern int setreuid(__uid_t real_uid, __uid_t eff_uid);
extern int setresuid(__uid_t real_uid, __uid_t eff_uid, __uid_t saved_uid);

#endif
