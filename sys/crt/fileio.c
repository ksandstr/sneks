
/* POSIX-like file I/O from system tasks. this is narrower in scope than the
 * full userspace POSIX experience.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sneks/sys/fs-defs.h>

#include <ccan/likely/likely.h>

#include "crt-private.h"


#define IOERR(n) unlikely(set_io_error((n)))


/* translate IDL stub status code to POSIX errno. */
static bool set_io_error(int n)
{
	if(likely(n == 0)) return false;
	else {
		printf("fileio.c: n=%d in return=%p\n", n, __builtin_return_address(0));
		if(n < 0) errno = -n;
		else {
			/* FIXME: translate L4.X2 error codes, such as EINTR */
			errno = EIO;
		}
		return true;
	}
}


int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if((flags & (O_CREAT | O_TMPFILE)) != 0) {
		va_list al;
		va_start(al, flags);
		mode = va_arg(al, mode_t);
		va_end(al);
	}
	L4_Word_t fd;
	int n = __fs_openat(__rootfs_tid, &fd, 0, path, flags, mode);
	if(IOERR(n)) return -1;

	/* TODO: install file descriptor somewhere */
	return fd;
}


int close(int fd)
{
	/* TODO: lookup fd somewhere */
	int n = __fs_close(__rootfs_tid, fd);
	return IOERR(n) ? -1 : 0;
}


long read(int fd, void *buf, size_t count)
{
	unsigned buf_len = count;
	int n = __fs_read(__rootfs_tid, buf, &buf_len, fd, ~0u, count);
	if(IOERR(n)) return -1;

	return buf_len;
}
