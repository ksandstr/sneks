
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ccan/compiler/compiler.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>
#include <ccan/intmap/intmap.h>

#include <sneks/process.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/file-defs.h>

#include <l4/types.h>

#include "private.h"


#define MAX_FD ((1 << 15) - 1)	/* TODO: get from sysconf() */


int __l4_last_errorcode = 0;	/* TODO: TSS ma bitch up */

fd_map_t fd_map;
static int first_free = 0, last_alloc = -1;


static bool invariants(void) {
	assert(first_free <= last_alloc + 1);
	assert(sintmap_empty(&fd_map) || last_alloc >= 0);
	assert(!sintmap_empty(&fd_map) || first_free == 0);
	assert(!sintmap_empty(&fd_map) || last_alloc == -1);
	assert(sintmap_get(&fd_map, first_free) == NULL);
	assert(last_alloc < 0 || sintmap_get(&fd_map, last_alloc) != NULL);
	return true;
}


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


/* find first free file descriptor at @low or greater. returns EMFILE when
 * there already exist FDs up to MAX_FD. this avoids O(n^2) scaling due to the
 * open(2) (etc.) requirement of using the lowest available fd number by
 * tracking first-free and last-alloc values. however, F_DUPFD that
 * always starts at the same position past first_free is still O(n^2).
 */
static int next_free_fd(int low)
{
	assert(invariants());
	assert(low >= 0);

	if(low > last_alloc) return low;
	if(low <= first_free) return first_free;

	sintmap_index_t ix = low - 1;
	void *prev = sintmap_after(&fd_map, &ix);
	assert(prev != NULL);	/* per last_alloc */
	if(ix > low) return low;	/* gap */
	else {
		while(ix <= MAX_FD) {
			if(sintmap_get(&fd_map, ++ix) == NULL) return ix;
		}
		errno = EMFILE;
		return -1;
	}
}


int __create_fd(int fd, L4_ThreadId_t server, intptr_t handle, int flags)
{
	assert(invariants());
	if(fd < 0) {
		fd = next_free_fd(0);
		if(fd < 0) return -1;
		assert(sintmap_get(&fd_map, fd) == NULL);
	}

	/* designated file descriptor street */
	struct fd_bits *bits = malloc(sizeof *bits);
	if(bits == NULL) goto Enomem;
	*bits = (struct fd_bits){
		.server = server, .handle = handle, .flags = flags,
	};
	if(!sintmap_add(&fd_map, fd, bits)) {
		free(bits);
		fd = -1;
	} else {
		last_alloc = max(last_alloc, fd);
		if(fd == first_free) {
			while(first_free <= MAX_FD) {
				if(sintmap_get(&fd_map, ++first_free) == NULL) break;
			}
		}
		assert(invariants());
	}
	return fd;

Enomem:
	errno = ENOMEM;
	return -1;
}


inline struct fd_bits *__fdbits(int fd)
{
	struct fd_bits *b;
	if(unlikely(fd < 0 || fd > MAX_FD
		|| (b = sintmap_get(&fd_map, fd), b == NULL)))
	{
		errno = EBADF;
		return NULL;
	}
	return b;
}


/* this isn't as much cold as init-only. */
COLD void __file_init(struct sneks_fdlist *fdlist)
{
	sintmap_init(&fd_map);
	assert(invariants());

	int err = 0, n = 0;
	if(fdlist != NULL && fdlist->next != 0) {
		int prev = fdlist->fd;
		do {
			if(fdlist->fd > prev) abort();	/* invalid fdlist */
			prev = fdlist->fd;
			/* NOTE: flags is always 0 because FD_CLOEXEC (O_CLOEXEC) doesn't
			 * carry through spawn or exec.
			 */
			n = __create_fd(fdlist->fd, fdlist->serv, fdlist->cookie, 0);
			fdlist = sneks_fdlist_next(fdlist);
		} while(n >= 0 && fdlist->next != 0);
		err = errno;
	}

	stdin = fdopen(0, "r");
	stdout = fdopen(1, "w");
	stderr = fdopen(2, "w");

	if(n < 0) {
		fprintf(stderr, "%s: __create_fd() failed, errno=%d\n", __func__, err);
		abort();
	}
}


int close(int fd)
{
	struct fd_bits *b = __fdbits(fd);
	if(b == NULL) return -1;
	else {
		int n = __io_close(b->server, b->handle);
		b->server = L4_nilthread;
		sintmap_del(&fd_map, fd);
		free(b);
		first_free = min(first_free, fd);
		if(fd == last_alloc) {
			while(last_alloc > 0) {
				if(sintmap_get(&fd_map, --last_alloc) != NULL) break;
			}
			first_free = min(first_free, last_alloc + 1);
		}
		assert(invariants());
		return NTOERR(n);
	}
}


long read(int fd, void *buf, size_t count)
{
	struct fd_bits *b = __fdbits(fd);
	if(b == NULL) return -1;
	if(count == 0) return 0;

	int n;
	unsigned length;
	__permit_recv_interrupt();
	do {
		length = count;
		n = __io_read(b->server, b->handle, count, -1, buf, &length);
	} while(n == -EAGAIN);
	__forbid_recv_interrupt();
	if(n == -EWOULDBLOCK) n = -EAGAIN;
	return NTOERR(n, length);
}


long write(int fd, const void *buf, size_t count)
{
	struct fd_bits *b = __fdbits(fd);
	if(b == NULL) return -1;
	if(count == 0) return 0;

	uint16_t rc;
	int n;
	do {
		n = __io_write(b->server, &rc, b->handle, -1, buf, count);
	} while(n == -EAGAIN);
	if(n == -EWOULDBLOCK) n = -EAGAIN;
	return NTOERR(n, rc);
}


off_t lseek(int fd, off_t offset, int whence)
{
	struct fd_bits *b = __fdbits(fd);
	if(b == NULL) return -1;
	switch(whence) {
		case SEEK_CUR: case SEEK_SET: case SEEK_END: break;
		default: errno = EINVAL; return -1;
	}

	int n = __file_seek(b->server, b->handle, &offset, whence);
	return NTOERR(n, offset);
}


int dup(int oldfd) {
	return dup2(oldfd, -1);
}


int dup2(int oldfd, int newfd)
{
	if(oldfd == newfd) return newfd;	/* no-op */
	return dup3(oldfd, newfd, 0);
}


/* TODO: make this signal-safe */
int dup3(int oldfd, int newfd, int flags)
{
	if(oldfd == newfd) {
		errno = EINVAL;
		return -1;
	}
	/* TODO: support FD_CLOEXEC, FD_RENAME_NP */
	if(flags != 0) {
		errno = EINVAL;
		return -1;
	}
	struct fd_bits *bits = __fdbits(oldfd);
	if(bits == NULL) return -1;
	int new_handle = -1, n = __io_dup(bits->server, &new_handle, bits->handle);
	if(n != 0) return NTOERR(n);
	if(newfd >= 0 && __fdbits(newfd) != NULL) close(newfd);
	n = __create_fd(newfd, bits->server, new_handle, flags);
	if(n < 0) __io_close(bits->server, new_handle);
	return n;
}


int fcntl(int fd, int cmd, ...)
{
	struct fd_bits *b = __fdbits(fd);
	if(b == NULL) return -1;
	va_list al; va_start(al, cmd);
	switch(cmd) {
		default: va_end(al); goto Einval;
		case F_DUPFD: {
			int low = va_arg(al, int);
			va_end(al);
			if(low < 0) goto Einval;
			int newfd = next_free_fd(low);
			if(newfd < 0) return -1;
			return dup2(fd, newfd);
		}
		case F_GETFL: {
			va_end(al);
			int old = 0;
			int n = __io_set_flags(b->server, &old, b->handle, 0, ~0l);
			return NTOERR(n, old);
		}
		case F_SETFL: {
			int val = va_arg(al, int);
			va_end(al);
			int foo, n = __io_set_flags(b->server, &foo, b->handle, val, ~val);
			return NTOERR(n);
		}
	}

	errno = ENOSYS;
	return -1;

Einval:
	errno = EINVAL;
	return -1;
}
