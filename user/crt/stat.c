
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sneks/api/io-defs.h>
#include <sneks/api/path-defs.h>

#include "private.h"

void __convert_statbuf(struct stat *dst, const struct sneks_path_statbuf *src)
{
#define F(name) .st_##name = src->st_##name
	*dst = (struct stat){
		/* fuck me harder. */
		F(dev), F(ino), F(mode), F(nlink), F(uid), F(gid),
		F(rdev), F(size), F(blksize), F(blocks),
		.st_atim = { src->st_atim.tv_sec, src->st_atim.tv_nsec },
		.st_mtim = { src->st_mtim.tv_sec, src->st_mtim.tv_nsec },
		.st_ctim = { src->st_ctim.tv_sec, src->st_ctim.tv_nsec },
	};
#undef F
}
#define convert_statbuf(a, b) __convert_statbuf((a), (b))

int stat(const char *pathname, struct stat *statbuf) {
	return fstatat(AT_FDCWD, pathname, statbuf, 0);
}

int fstat(int fd, struct stat *statbuf)
{
	struct fd_bits *bits = __fdbits(fd);
	if(bits == NULL) { errno = EBADF; return -1; }
	struct fd_ext *ext = __fdext(fd);
	if(ext != NULL) {
		*statbuf = ext->st;
		return 0;
	} else {
		struct sneks_path_statbuf st;
		int n = __path_stat_handle(bits->server, bits->handle, &st);
		if(n == 0) convert_statbuf(statbuf, &st);
		return NTOERR(n);
	}
}

int lstat(const char *pathname, struct stat *statbuf) {
	return fstatat(AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}


int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags)
{
	if(flags & ~AT_SYMLINK_NOFOLLOW) { errno = ENOSYS; return -1; }

	struct resolve_out r;
	int n = __resolve(&r, dirfd, pathname, flags);
	if(n == 0) {
		struct sneks_path_statbuf st;
		n = __path_stat_object(r.server, r.object, r.cookie, &st);
		if(n == 0) convert_statbuf(statbuf, &st);
	}
	return NTOERR(n);
}
