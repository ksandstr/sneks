#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/minmax/minmax.h>
#include <l4/types.h>
#include "private.h"

static ssize_t fd_read(void *cookie, char *buf, size_t size) {
	ssize_t n;
	do n = read((int)cookie, buf, size); while(n < 0 && errno == EINTR);
	return max_t(ssize_t, n, 0);
}

static ssize_t fd_write(void *cookie, const char *buf, size_t size) {
	ssize_t n;
	do n = write((int)cookie, buf, size); while(n < 0 && errno == EINTR);
	return max_t(ssize_t, n, 0);
}

static int fd_seek(void *cookie, off64_t *offset, int whence) {
	off_t n = lseek((int)cookie, *offset, whence);
	if(n >= 0) *offset = n;
	return (int)n;
}

static int fd_close(void *cookie) {
	return close((int)cookie) < 0 ? EOF : 0;
}

FILE *fdopen(int fd, const char *mode) {
	FILE *f = fopencookie((void *)fd, mode, (cookie_io_functions_t){ .write = &fd_write, .read = &fd_read, .close = &fd_close, .seek = &fd_seek });
	if(f != NULL) setvbuf(f, NULL, isatty(fd) ? _IOLBF : _IOFBF, 0);
	return f;
}

FILE *fopen(const char *path, const char *modestr)
{
	int flags, fd, major = 0, plus = false;
	for(int i=0; modestr[i] != '\0'; i++) {
		switch(modestr[i]) {
			case 'r': case 'w': case 'a': major = modestr[i]; break;
			case '+': plus = true; break;
			/* ignore the rest */
		}
	}
	switch(major) {
		case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
		case 'w': flags = O_CREAT | (plus ? O_RDWR : O_WRONLY); break;
		case 'a': flags = O_CREAT | (plus ? O_RDWR | O_APPEND : O_WRONLY); break;
		default: errno = EINVAL; return NULL;
	}
	if(fd = open(path, flags, 0660), fd < 0) return NULL;
	if(major == 'w') { /* TODO: ftruncate */ }
	if(major == 'a') lseek(fd, 0, SEEK_END);
	FILE *f = fdopen(fd, modestr);
	if(f == NULL) close(fd);
	return f;
}
