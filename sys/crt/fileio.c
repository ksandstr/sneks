/* some POSIX-like file I/O from system tasks. */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <ccan/array_size/array_size.h>
#include <ccan/minmax/minmax.h>
#include <ccan/str/str.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <sneks/systask.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/path-defs.h>
#include "private.h"

#define IOERR(n) set_io_errno((n))
#define VALID(fd) ((fd) < ARRAY_SIZE(filetab) && \
	!L4_IsNilThread(filetab[(fd)].server))

struct sys_file {
	int handle;
	L4_ThreadId_t server; /* nil for vacant fd */
};

/* 13 descriptors should be enough for every systask, barring leaks. */
static struct sys_file filetab[16] = {
	[0] = { }, [1] = { }, [2] = { },
};

static bool set_io_errno(int n)
{
	if(n == 0) return false;
	log_info("n=%d in return=%p", n, __builtin_return_address(0));
	if(n < 0) errno = -n; else errno = EIO; /* TODO: translate L4 error? */
	return true;
}

static int get_free_file(void) {
	for(int i=3; i < ARRAY_SIZE(filetab); i++) {
		if(L4_IsNilThread(filetab[i].server)) return i;
	}
	return -1;
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
	while(*path == '/') path++;

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
	int fd = get_free_file();
	if(fd < 0) { errno = EMFILE; return -1; }
	n = __file_open(server, &filetab[fd].handle, object, cookie, flags);
	if(IOERR(n)) return -1;
	actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;
	assert(!L4_IsNilThread(server));
	filetab[fd].server = server;
	assert(VALID(fd));
	return fd;
}

int close(int fd)
{
	if(!VALID(fd)) { errno = EBADF; return -1; }
	int n = __io_close(filetab[fd].server, filetab[fd].handle);
	return IOERR(n) ? -1 : 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
	if(!VALID(fd)) { errno = EBADF; return -1; }
	unsigned buf_len = min_t(size_t, count, INT_MAX);
	int n = __io_read(filetab[fd].server, filetab[fd].handle, buf_len, -1, buf, &buf_len);
	return IOERR(n) ? -1 : buf_len;
}

off_t lseek(int fd, off_t offset, int whence)
{
	if(!VALID(fd)) { errno = EBADF; return -1; }
	int64_t off = offset;
	int n = __file_seek(filetab[fd].server, filetab[fd].handle, &off, whence);
	return IOERR(n) ? -1 : off;
}
