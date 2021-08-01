
#include <string.h>
#include <assert.h>
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ccan/minmax/minmax.h>

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

	char *pathbuf = bufsize < PATH_MAX ? alloca(PATH_MAX + 1) : buf;
	int pathlen = 0;
	n = __dir_readlink(r.server, pathbuf, &pathlen, r.object, r.cookie);
	if(n != 0) return NTOERR(n);
	assert(pathbuf[pathlen] == '\0');

	if(bufsize < PATH_MAX) memcpy(buf, pathbuf, min_t(size_t, pathlen, bufsize));
	if(pathlen < bufsize) buf[pathlen] = '\0';	/* mr. nice guy */
	return min_t(size_t, pathlen, bufsize);

Einval: errno = EINVAL; return -1;
}
