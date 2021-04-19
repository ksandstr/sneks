
#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stdint.h>

/* TODO: selectively include size_t, ssize_t, uid_t, gid_t, off_t, and
 * pid_t
 */
#include <sys/types.h>

/* TODO: get these from API decls once find-idl-defs.pl understands the
 * ubiquitous #include <stdio.h>.
 */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif


typedef unsigned int useconds_t;

struct timeval;


extern int getpagesize(void);
extern pid_t getpid(void);

extern long sysconf(int name);

extern int brk(void *addr);
extern void *sbrk(intptr_t increment);

extern int pause(void);


extern int close(int fd);
extern long read(int fd, void *buf, size_t count);
extern long write(int fd, const void *buf, size_t count);
extern off_t lseek(int fd, off_t offset, int whence);

extern int dup(int oldfd);
extern int dup2(int oldfd, int newfd);
#ifdef _GNU_SOURCE
extern int dup3(int oldfd, int newfd, int flags);
#endif

extern int pipe(int pipefd[2]);
#ifdef _GNU_SOURCE
extern int pipe2(int pipefd[2], int flags);
#endif

extern pid_t fork(void);
extern pid_t wait(int *status_p);


/* sysconf() names as far as sneks knows of 'em. */
enum {
	_SC_PAGESIZE,
#define _SC_PAGESIZE _SC_PAGESIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
	_SC_NPROCESSORS_ONLN,
};


extern unsigned int sleep(unsigned int seconds);
extern int usleep(useconds_t usec);

extern uid_t getuid(void);
extern uid_t geteuid(void);

extern int setuid(uid_t uid);
extern int seteuid(uid_t uid);
extern int setreuid(uid_t real_uid, uid_t eff_uid);
extern int setresuid(uid_t real_uid, uid_t eff_uid, uid_t saved_uid);


extern char *getcwd(char *buf, size_t size);
#ifdef _GNU_SOURCE
extern char *get_current_dir_name(void);
#endif

extern int chdir(const char *path);
extern int fchdir(int dirfd);


extern int select(
	int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout);

extern void FD_CLR(int fd, fd_set *set);
extern int FD_ISSET(int fd, fd_set *set);
extern void FD_SET(int fd, fd_set *set);
extern void FD_ZERO(fd_set *set);

#define __UINTPTR_BITS (sizeof(uintptr_t) * 8)

#define FD_CLR(fd, set) do { \
		int __fd = (fd); \
		((set)->__w[__fd / __UINTPTR_BITS] &= ~(1ul << (__fd % __UINTPTR_BITS))); \
	} while(0)
#define FD_ISSET(fd, set) ({ \
		int __fd = (fd); \
		!!((set)->__w[__fd / __UINTPTR_BITS] & (1ul << (__fd % __UINTPTR_BITS))); \
	})
#define FD_SET(fd, set) do { \
		int __fd = (fd); \
		((set)->__w[__fd / __UINTPTR_BITS] |= 1ul << (__fd % __UINTPTR_BITS)); \
	} while(0)

#endif
