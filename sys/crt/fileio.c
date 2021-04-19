
/* POSIX-like file I/O from system tasks. this is narrower in scope than the
 * full userspace POSIX experience.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>
#include <ccan/str/str.h>

#include <l4/types.h>
#include <l4/thread.h>

#include <sneks/api/file-defs.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/io-defs.h>

#include "private.h"


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


/* all paths to sys/crt open(2) must be absolute, it will only open regular
 * files, and accmode must be O_RDONLY.
 */
int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if((flags & (O_CREAT | O_TMPFILE)) != 0) {
		va_list al;
		va_start(al, flags);
		mode = va_arg(al, mode_t);
		va_end(al);
	}
	if((flags & O_ACCMODE) != O_RDONLY || path == NULL || path[0] != '/') {
		errno = -EINVAL;
		return -1;
	}

	bool booted;
	L4_ThreadId_t rootfs_tid = __get_rootfs(&booted);
	if(!booted && strstarts(path, "/initrd/")) path += 8;

	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(rootfs_tid, &object, &server.raw,
		&ifmt, &cookie, 0, path, flags | mode);
	if(IOERR(n)) return -1;
	if((ifmt & SNEKS_PATH_S_IFMT) != SNEKS_PATH_S_IFREG) {
		errno = EBADF;	/* bizarre, but works */
		return -1;
	}

	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_ThreadId_t actual = L4_nilthread;
	int fd;
	n = __file_open(server, &fd, object, cookie, flags);
	if(IOERR(n)) return -1;
	actual = L4_ActualSender();

	/* TODO: install file descriptor somewhere, since we'll certainly want to
	 * access files outside the root filesystem also; and the root filesystem
	 * will change during boot and perhaps also later.
	 */
	assert(L4_IsNilThread(actual) || L4_SameThreads(actual, server));
	assert(L4_SameThreads(server, rootfs_tid));

	return fd;
}


int close(int fd)
{
	int n = __io_close(__get_rootfs(NULL), fd);
	return IOERR(n) ? -1 : 0;
}


long read(int fd, void *buf, size_t count)
{
	unsigned buf_len = min_t(size_t, count, INT_MAX);
	int n = __io_read(__get_rootfs(NULL), fd, buf_len, -1, buf, &buf_len);
	return IOERR(n) ? -1 : buf_len;
}
