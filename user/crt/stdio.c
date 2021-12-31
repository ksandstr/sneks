
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>

#include "private.h"


static long fd_read(void *cookie, char *buf, size_t size)
{
	int n;
	do {
		n = read((int)cookie, buf, size);
	} while(n < 0 && errno == EINTR);
	return max_t(long, n, 0);
}


static long fd_write(void *cookie, const char *buf, size_t size)
{
	int n;
	do {
		n = write((int)cookie, buf, size);
	} while(n < 0 && errno == EINTR);
	return max_t(long, n, 0);
}


static int fd_seek(void *cookie, off64_t *offset, int whence) {
	off_t n = lseek((int)cookie, *offset, whence);
	if(n >= 0) *offset = n;
	return (int)n;
}


static int fd_close(void *cookie) {
	return 0;
}


FILE *fdopen(int fd, const char *mode)
{
	FILE *f = fopencookie((void *)fd, mode, (cookie_io_functions_t){
		.write = &fd_write, .read = &fd_read,
		.close = &fd_close, .seek = &fd_seek,
	});
	/* TODO: use _IOLBF when isatty(), but isatty() isn't here yet. */
	setvbuf(f, NULL, _IOFBF, 0);
	return f;
}
