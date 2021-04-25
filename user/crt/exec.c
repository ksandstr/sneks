
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>


int execve(const char *pathname, char *const argv[], char *const envp[])
{
	errno = ENOSYS;
	return -1;
}


int execl(const char *pathname, const char *arg, ... /* (char *)NULL */)
{
	errno = ENOSYS;
	return -1;
}


int execlp(const char *file, const char *arg, ... /* (char *)NULL */)
{
	errno = ENOSYS;
	return -1;
}


int execle(const char *pathname, const char *arg,
	... /* (char *)NULL, char *const envp[] */)
{
	errno = ENOSYS;
	return -1;
}


int execv(const char *pathname, char *const argv[])
{
	errno = ENOSYS;
	return -1;
}


int execvp(const char *file, char *const argv[])
{
	errno = ENOSYS;
	return -1;
}


#ifdef _GNU_SOURCE
int execvpe(const char *file, char *const argv[], char *const envp[])
{
	errno = ENOSYS;
	return -1;
}
#endif


int fexecve(int fd, char *const argv[], char *const envp[])
{
	errno = ENOSYS;
	return -1;
}


int execveat(int dirfd, const char *pathname,
	char *const argv[], char *const envp[], int flags)
{
	errno = ENOSYS;
	return -1;
}
