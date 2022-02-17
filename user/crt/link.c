#include <string.h>
#include <assert.h>
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ccan/minmax/minmax.h>

#include <sneks/api/path-defs.h>
#include <sneks/api/directory-defs.h>

#include "private.h"

ssize_t readlink(const char *restrict path, char *restrict buf, size_t bufsize) {
	return readlinkat(AT_FDCWD, path, buf, bufsize);
}

ssize_t readlinkat(int dirfd, const char *restrict path,
	char *restrict buf, size_t bufsize)
{
	if(bufsize == 0) goto Einval;

	struct resolve_out r;
	int n = __resolve(&r, dirfd, path, AT_SYMLINK_NOFOLLOW);
	if(n != 0) return NTOERR(n);
	if(r.ifmt >> 12 != SNEKS_DIRECTORY_DT_LNK) goto Einval;

	const size_t path_max = SNEKS_PATH_PATH_MAX;
	char *pathbuf = bufsize < path_max ? alloca(path_max + 1) : buf;
	int pathlen = 0;
	n = __dir_readlink(r.server, pathbuf, &pathlen, r.object, r.cookie);
	if(n != 0) return NTOERR(n);
	size_t n_wrote = min_t(size_t, pathlen, bufsize);
	if(bufsize < path_max) {
		/* copy back from alloca'd buffer */
		memcpy(buf, pathbuf, n_wrote);
	}
	return n_wrote;

Einval: errno = EINVAL; return -1;
}
