
/* POSIX interfaces that handle path names such as openat(), where an open()
 * exists that defaults to the current directory.
 */

#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/thread.h>

#include <sneks/sysinfo.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/directory-defs.h>
#include <sneks/api/dev-defs.h>
#include <sneks/api/io-defs.h>

#include "private.h"


int __resolve(
	struct resolve_out *res,
	int dirfd, const char *pathname, int flags)
{
	struct fd_bits *cwd,
		rootfs = { .server = __the_sysinfo->api.rootfs, .handle = 0 };
	if(pathname[0] == '/') {
		while(pathname[0] == '/') pathname++;
		cwd = &rootfs;
	} else {
		cwd = __fdbits(dirfd == AT_FDCWD ? __cwd_fd : dirfd);
		if(cwd == NULL) return -EBADF;
	}

	return __path_resolve(cwd->server,
		&res->object, &res->server.raw, &res->ifmt, &res->cookie,
		cwd->handle, pathname, flags);
}


int openat(int dirfd, const char *pathname, int flags, ...)
{
	if(pathname == NULL) {
		errno = EINVAL;
		return -1;
	}
	if(flags & O_CREAT) {
		/* TODO */
		errno = ENOSYS;
		return -1;
	}

	struct resolve_out r;
	int n = __resolve(&r, dirfd, pathname, flags);
	if(n != 0) return NTOERR(n);

	/* set VS/AS to recover actual server tid, for when Path::resolve hands
	 * out a propagator
	 */
	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_ThreadId_t actual = L4_nilthread;
	int handle;
	switch(r.ifmt & S_IFMT) {
		case S_IFCHR: case S_IFBLK:
			n = __dev_open(r.server, &handle, r.object, r.cookie, flags);
			actual = L4_ActualSender();
			break;
		case S_IFREG: case S_IFIFO: case S_IFSOCK:
			n = __file_open(r.server, &handle, r.object, r.cookie, flags);
			if((r.ifmt & S_IFMT) != S_IFREG) {
				/* fifos and sockets generally won't be handled by the
				 * Sneks::File provider. allow propagated handling when
				 * resolve's `server' output wasn't the socket or fifo server.
				 */
				actual = L4_ActualSender();
			}
			break;
		case S_IFDIR:
			n = __dir_opendir(r.server, &handle, r.object, r.cookie, flags);
			actual = L4_ActualSender();
			break;
		case S_IFLNK:
			/* symbolic links returned from Sneks::Path/resolve are a result
			 * of O_NOFOLLOW in @flags, so this status should be returned.
			 */
			errno = ELOOP;
			return -1;
		default: errno = ENOSYS; return -1;
	}
	if(n != 0) return NTOERR(n);
	if(!L4_IsNilThread(actual)) r.server = actual;

	int fflags = 0;
	if(flags & O_CLOEXEC) fflags |= FD_CLOEXEC;
	int fd = __create_fd(-1, r.server, handle, fflags);
	if(fd < 0) {
		__io_close(r.server, handle);
		errno = -fd;
		return -1;
	}

	return fd;
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


int unlink(const char *path) {
	errno = ENOSYS;
	return -1;
}


int unlinkat(int fdcwd, const char *path, int flags) {
	errno = ENOSYS;
	return -1;
}
