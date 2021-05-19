
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <alloca.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/darray/darray.h>

#include <sneks/api/path-defs.h>

#include "private.h"


int __cwd_fd = -1;


char *getcwd(char *buf, size_t buf_size)
{
	if(buf_size == 0) {
		/* Linux manpages say this should happen iff also buf != NULL, but the
		 * POSIX ones never mention that. we'll go with POSIX.
		 */
		errno = EINVAL;
		return NULL;
	}
	char *curdir = get_current_dir_name();
	if(curdir == NULL) return NULL;
	size_t len = strlen(curdir) + 1;
	if(len > buf_size) {
		errno = ERANGE;
		return NULL;
	}
	memcpy(buf, curdir, len);
	free(curdir);
	return buf;
}


/* TODO: remove darray, catch ENOMEM everywhere */
char *get_current_dir_name(void)
{
	struct fd_bits *bits = __fdbits(__cwd_fd);
	if(bits == NULL || bits->handle == 0) {
		errno = ENOENT;
		return NULL;
	}

	/* fast mode. */
	char *ret;
	darray(char) buf = darray_new();
	darray_realloc(buf, SNEKS_PATH_PATH_MAX + 1);
	buf.item[0] = '/';
	int n = __path_get_path(bits->server, buf.item + 1, bits->handle, "");
	if(n != 0 && n != -ENAMETOOLONG) goto fail;
	else if(n == 0) {
		buf.size = strlen(buf.item);
		goto end;
	}

	/* the long way around. */
	unsigned object = 0;
	L4_Word_t cookie = 0;
	L4_ThreadId_t server = L4_nilthread;
	char *frag = alloca(SNEKS_PATH_PATH_MAX + 1);	/* thicc stacc */
	frag[0] = '/';
	n = __path_get_path_fragment(bits->server, &frag[1],
		&object, &server.raw, &cookie, bits->handle);
	for(;;) {
		if(n != 0) goto fail;
		darray_prepend_string(buf, frag);
		if(L4_IsNilThread(server)) break;
		n = __path_get_path_fragment(server, &frag[1],
			&object, &server.raw, &cookie, 0);
	}

end:
	ret = realloc(buf.item, buf.size + 1);
	ret[buf.size] = '\0';
	return ret;

fail:
	darray_free(buf);
	NTOERR(n);
	return NULL;
}


int chdir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if(fd < 0) return -1;
	else {
		if(__cwd_fd >= 0) close(__cwd_fd);
		__cwd_fd = fd;
		return 0;
	}
}


static bool is_directory_fd(int fd) {
#if 1
	return true;
#else
	/* NOTE: we do want this, but right now it means resolving the initrd
	 * prefix issue or init won't start, so this should be restored at that
	 * point.
	 */
	int file = openat(fd, ".", 0);	/* TODO: use O_CLOEXEC */
	if(file >= 0) close(file);
	return file >= 0;
#endif
}


int fchdir(int dirfd)
{
	if(!is_directory_fd(dirfd)) {
		errno = ENOTDIR;
		return -1;
	}
	int copy = dup(dirfd);
	if(copy < 0) return -1;
	else {
		if(__cwd_fd >= 0) close(__cwd_fd);
		__cwd_fd = copy;
		return 0;
	}
}
