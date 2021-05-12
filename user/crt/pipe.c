
#include <fcntl.h>
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
	const L4_ThreadId_t server = __the_sysinfo->posix.pipe;
	int rdwr[2], n = __pipe_pipe(server, &rdwr[0], &rdwr[1], flags);
	if(n != 0) return NTOERR(n);

	int fd_flags = 0;
	if(flags & O_CLOEXEC) fd_flags |= FD_CLOEXEC;
	for(int i=0; i < 2; i++) {
		pipefd[i] = __create_fd(-1, server, rdwr[i], fd_flags);
		if(pipefd[i] < 0) {
			if(i > 0) close(pipefd[0]);
			__io_close(server, rdwr[0]);
			__io_close(server, rdwr[1]);
			errno = -pipefd[i];
			return -1;
		}
	}

	return 0;
}
