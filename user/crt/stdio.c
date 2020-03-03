
/* (no longer derived from fake_stdio.c of mung as of 20190516.) */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>

#include "private.h"


static long fd_write(void *cookie, const char *buf, size_t size)
{
	int n;
	do {
		n = write((int)cookie, buf, size);
	} while(n < 0 && errno == EINTR);
	return max_t(long, n, 0);
}


FILE *fdopen(int fd, const char *mode)
{
	return fopencookie((void *)fd, mode, (cookie_io_functions_t){
		.write = &fd_write,
		/* TODO: add read, close */
	});
}
