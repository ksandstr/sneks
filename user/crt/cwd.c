
#include <errno.h>
#include <unistd.h>


int chdir(const char *path)
{
	errno = ENOSYS;
	return -1;
}


int fchdir(int dirfd)
{
	errno = ENOSYS;
	return -1;
}
