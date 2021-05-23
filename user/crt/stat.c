
#include <errno.h>
#include <sys/stat.h>


int stat(const char *pathname, struct stat *statbuf)
{
	errno = ENOSYS;
	return -1;
}


int fstat(int fd, struct stat *statbuf)
{
	errno = ENOSYS;
	return -1;
}


int lstat(const char *pathname, struct stat *statbuf)
{
	errno = ENOSYS;
	return -1;
}


int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags)
{
	errno = ENOSYS;
	return -1;
}
