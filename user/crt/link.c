
#include <errno.h>
#include <unistd.h>


ssize_t readlink(const char *restrict path,
	char *restrict buf, size_t bufsize)
{
	errno = ENOSYS;
	return -1;
}


ssize_t readlinkat(int fd, const char *restrict path,
	char *restrict buf, size_t bufsize)
{
	errno = ENOSYS;
	return -1;
}
