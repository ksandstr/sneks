
/* (no longer derived from fake_stdio.c of mung as of 20190516.) */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>

#include <l4/types.h>

#include "io-defs.h"
#include "private.h"


static long fd_write(void *cookie, const char *buf, size_t size)
{
	int fd = (int)cookie;
	assert(IS_FD_VALID(fd));
	if(size > SNEKS_IO_IOSEG_MAX) size = SNEKS_IO_IOSEG_MAX;
	uint16_t ret;
	int n = __io_write(FD_SERVICE(fd), &ret, FD_COOKIE(fd), (void *)buf, size);
	return NTOERR(n, (int)ret);
}


FILE *fdopen(int fd, const char *mode)
{
	static const cookie_io_functions_t fd_io_funcs = {
		.write = &fd_write,
	};
	return fopencookie((void *)fd, mode, fd_io_funcs);
}
