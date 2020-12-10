
#include <unistd.h>
#include <errno.h>

#include <sneks/sysinfo.h>
#include <sneks/api/pipe-defs.h>
#include <sneks/api/io-defs.h>

#include "private.h"


int pipe(int pipefd[2]) {
	return pipe2(pipefd, 0);
}


int pipe2(int pipefd[2], int flags)
{
	L4_Word_t rdwr[2];
	int n = __pipe_pipe(__the_sysinfo->posix.pipe, &rdwr[0], &rdwr[1], flags);
	if(n != 0) return NTOERR(n);

	void *ctx = NULL;
	for(int i=0; i < 2; i++) {
		pipefd[i] = __alloc_fd_bits(&ctx, -1,
			__the_sysinfo->posix.pipe, rdwr[i], flags);
		if(pipefd[i] < 0) {
			errno = -pipefd[i];
			if(i > 0) close(pipefd[0]);
			__io_close(__the_sysinfo->posix.pipe, rdwr[0]);
			__io_close(__the_sysinfo->posix.pipe, rdwr[1]);
			return -1;
		}
	}

	return 0;
}
