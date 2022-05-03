#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

typedef unsigned int useconds_t;

struct timeval;

extern int getpagesize(void);
extern pid_t getpid(void);
extern long sysconf(int name);
extern int brk(void *addr);
extern void *sbrk(intptr_t increment);
extern int pause(void);

extern int close(int fd);
extern ssize_t read(int fd, void *buf, size_t count);
extern ssize_t write(int fd, const void *buf, size_t count);
extern off_t lseek(int fd, off_t offset, int whence);
extern int isatty(int fd);

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

/* exec family. */
extern int execve(const char *pathname, char *const argv[], char *const envp[]);
extern int execl(const char *pathname, const char *arg, ... /* (char *)NULL */);
extern int execlp(const char *file, const char *arg, ... /* (char *)NULL */);
extern int execle(const char *pathname, const char *arg, ... /* (char *)NULL, char *const envp[] */);
extern int execv(const char *pathname, char *const argv[]);
extern int execvp(const char *file, char *const argv[]);
#ifdef _GNU_SOURCE
extern int execvpe(const char *file, char *const argv[], char *const envp[]);
#endif
extern int fexecve(int fd, char *const argv[], char *const envp[]);
extern int execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags); /* linux-specific but we'll allow it. */
/* TODO: join AT_SYMLINK_NOFOLLOW's proper def somewhere else */
#define AT_EMPTY_PATH (1 << 30)
#define AT_SYMLINK_NOFOLLOW (1 << 29)

enum {
	_SC_PAGESIZE,
#define _SC_PAGESIZE _SC_PAGESIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
	_SC_NPROCESSORS_ONLN,
	_SC_OPEN_MAX,
};

extern _Noreturn void _exit(int);

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

extern int unlink(const char *path);
extern int unlinkat(int fdcwd, const char *path, int flags);

extern ssize_t readlink(const char *restrict path, char *restrict buf, size_t bufsize);
extern ssize_t readlinkat(int fd, const char *restrict path, char *restrict buf, size_t bufsize);

#endif
