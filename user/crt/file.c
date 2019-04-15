
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/compiler/compiler.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>
#include <sneks/process.h>

#include <l4/types.h>

#include "private.h"


/* NOTE: statically allocated when max_valid_fd < 8. */
struct __sneks_file *__files = NULL;
int __max_valid_fd = -1;	/* valid as in memory, not IS_FD_VALID() */

int __l4_last_errorcode = 0;	/* TODO: TSS this up */


int __idl2errno(int n, ...)
{
	__l4_last_errorcode = n;
	if(likely(n == 0)) {
		va_list al; va_start(al, n);
		int ret = va_arg(al, int);
		va_end(al);
		return ret;
	} else if(n < 0) {
		errno = -n;
		return -1;
	} else {
		n >>= 1;	/* skip recv/send side. */
		/* NOTE: some of these will deviate from the semantics of the syscall
		 * wrappers they'll be used in. adjust them to rarer errno values as
		 * that happens.
		 */
		static const int l4_to_errno[] = {
			/* codes where partner wasn't involved yet, so signaled only to
			 * current thread.
			 */
			[0] = 0,
			[1] = ETIMEDOUT,	/* timed out */
			[2] = ESRCH,		/* non-existing partner */
			[3] = EINTR,		/* canceled by other thread (w/ exregs) */
			/* codes signaled to current thread and partner both. */
			[4] = EOVERFLOW,	/* message overflow (no strxfer buffers) */
			[5] = ETIMEDOUT,	/* timed out during xfer fault in own space */
			[6] = ETIMEDOUT,	/* same, in partner's space */
			[7] = EINTR,		/* aborted during strxfer (w/ exregs) */
		};
		errno = n < ARRAY_SIZE(l4_to_errno) ? l4_to_errno[n] : EIO;
		return -1;
	}
}


/* this isn't as much cold as init-only. */
COLD void __file_init(struct sneks_fdlist *fdlist)
{
	if(fdlist == NULL) return;
	assert(__max_valid_fd < 0);

	static struct __sneks_file first_files[8];
	if(fdlist == NULL || fdlist->fd < ARRAY_SIZE(first_files)) {
		__max_valid_fd = ARRAY_SIZE(first_files) - 1;
		__files = first_files;
		for(int i=0; i < ARRAY_SIZE(first_files); i++) {
			first_files[i] = (struct __sneks_file){ };
		}
	} else {
		__max_valid_fd = fdlist->fd;
		__files = calloc(__max_valid_fd + 1, sizeof *__files);
		if(__files == NULL) abort();	/* callstack breadcrumbs > segfault */
	}
	int prev = fdlist->fd;
	while(fdlist->next != 0) {
		if(fdlist->fd > prev) abort();	/* invalid fdlist */
		prev = fdlist->fd;
		struct __sneks_file *f = &__files[fdlist->fd];
		f->service = fdlist->serv;
		f->cookie = fdlist->cookie;
		fdlist = sneks_fdlist_next(fdlist);
	}

	stdin = fdopen(0, "r");
	stdout = fdopen(1, "w");
	stderr = fdopen(2, "w");
}


int close(int fd)
{
	errno = ENOSYS;
	return -1;
}


int pipe(int pipefd[2]) {
	return pipe2(pipefd, 0);
}


int pipe2(int pipefd[2], int flags)
{
	if(flags != 0) {
		errno = EINVAL;
		return -1;
	}

	/* TODO */
	errno = ENOSYS;
	return -1;
}


long read(int fd, void *buf, size_t count)
{
	errno = ENOSYS;
	return -1;
}


long write(int fd, const void *buf, size_t count)
{
	errno = ENOSYS;
	return -1;
}


uint64_t lseek(int fd, uint64_t offset, int whence)
{
	errno = ENOSYS;
	return -1;
}


int dup(int oldfd)
{
	errno = ENOSYS;
	return -1;
}


int dup2(int oldfd, int newfd)
{
	errno = ENOSYS;
	return -1;
}


int dup3(int oldfd, int newfd, int flags)
{
	errno = ENOSYS;
	return -1;
}


int fcntl(int fd, int cmd, ...)
{
	errno = ENOSYS;
	return -1;
}
