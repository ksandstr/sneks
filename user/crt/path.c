
/* POSIX interfaces that handle path names. */

#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>


int openat(int dirfd, const char *pathname, int flags, ...)
{
	errno = -ENOSYS;
	return -1;
}


int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	if(flags & (O_CREAT | O_TMPFILE)) {
		va_list al;
		va_start(al, flags);
		mode = va_arg(al, mode_t);
		va_end(al);
	}
	return openat(AT_FDCWD, pathname, flags, mode);
}
