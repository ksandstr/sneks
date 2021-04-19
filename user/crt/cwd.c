
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include "private.h"


int __cwd_fd = -1;


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


char *getcwd(char *buf, size_t size) {
	strncpy(buf, ".", size);
	return buf;
}


char *get_current_dir_name(void) {
	return strdup(".");
}
