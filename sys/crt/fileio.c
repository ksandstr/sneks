
/* POSIX-like file I/O from system tasks. this is narrower in scope than the
 * full userspace POSIX experience.
 */

#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>


int open(const char *path, int flags, ...)
{
	errno = ENOSYS;
	return -1;
}


int close(int fd)
{
	return 0;
}


long read(int fd, void *buf, size_t count)
{
	errno = ENOSYS;
	return -1;
}
