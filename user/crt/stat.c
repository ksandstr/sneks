
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sneks/api/io-defs.h>

#include "private.h"


static void convert_statbuf(struct stat *dst,
	const struct sneks_io_statbuf *src)
{
#define F(name) .st_##name = src->st_##name
	*dst = (struct stat){
		/* fuck me harder. */
		F(dev), F(ino), F(mode), F(nlink), F(uid), F(gid),
		F(rdev), F(size), F(blksize), F(blkcnt),
		.st_atim = { src->st_atim.tv_sec, src->st_atim.tv_nsec },
		.st_mtim = { src->st_mtim.tv_sec, src->st_mtim.tv_nsec },
		.st_ctim = { src->st_ctim.tv_sec, src->st_ctim.tv_nsec },
	};
#undef F
}


int stat(const char *pathname, struct stat *statbuf)
{
	errno = ENOSYS;
	return -1;
}


int fstat(int fd, struct stat *statbuf)
{
	struct fd_bits *bits = __fdbits(fd);
	if(bits == NULL) { errno = EBADF; return -1; }
	struct sneks_io_statbuf st;
	int n = __io_stat_handle(bits->server, bits->handle, &st);
	if(n == 0) convert_statbuf(statbuf, &st);
	return NTOERR(n);
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
