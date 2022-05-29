
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

static bool isfs(int ifmt) {
	switch(ifmt & S_IFMT) {
		case S_IFREG: case S_IFDIR: case S_IFLNK: return true;
		default: return false;
	}
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	if(pathname == NULL) { errno = EINVAL; return -1; }
	if(flags & O_CREAT) { errno = ENOSYS; return -1; }
	int n, handle, fflags = 0, fd;
	struct resolve_out r;
	if(n = __resolve(&r, dirfd, pathname, flags), n != 0) return NTOERR(n);
	struct stat st;
	if(!isfs(r.ifmt)) {
		struct sneks_path_statbuf pst;
		if(n = __path_stat_object(r.server, r.object, r.cookie, &pst), n != 0) return NTOERR(n);
		__convert_statbuf(&st, &pst);
	}
	L4_Set_VirtualSender(L4_nilthread); assert(L4_IsNilThread(L4_ActualSender()));
	if(n = __file_open(r.server, &handle, r.object, r.cookie, flags), n != 0) return NTOERR(n);
	L4_ThreadId_t actual = L4_ActualSender();
	if(L4_IsNilThread(actual)) actual = r.server;
	if(flags & O_CLOEXEC) fflags |= FD_CLOEXEC;
	if(fd = __create_fd_ext(-1, actual, handle, fflags, isfs(r.ifmt) ? NULL : &st), fd < 0) {
		__io_close(actual, handle);
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
