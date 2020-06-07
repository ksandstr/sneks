
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ccan/compiler/compiler.h>
#include <ccan/array_size/array_size.h>
#include <ccan/bitmap/bitmap.h>
#include <ccan/minmax/minmax.h>
#include <ccan/likely/likely.h>

#include <sneks/rbtree.h>
#include <sneks/process.h>
#include <ukernel/rangealloc.h>

#include <l4/types.h>

#include "private.h"
#include "io-defs.h"


#define CHUNKSZ (1 << 9)


struct fdchunk
{
	struct rb_node rb;	/* in fdchunk_tree, per step asc */
	int step;	/* covers FDs [step << 9, (step + 1) << 9) */

	struct fd fds[CHUNKSZ];
	BITMAP_DECLARE(occ, CHUNKSZ);
};


static struct rangealloc *ra_fdbits;
static struct rb_root fdchunk_tree = RB_ROOT;

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


/* gets chunk for @fd, reusing one in *@ctx if not NULL. */
static PURE_FUNCTION struct fdchunk *get_chunk(void **ctx, int fd)
{
	struct fdchunk *c = ctx != NULL ? *ctx : NULL;
	if(c != NULL && c->step == fd >> 9) return c;

	struct rb_node *n = fdchunk_tree.rb_node;
	int key = fd / CHUNKSZ;
	while(n != NULL) {
		c = rb_entry(n, struct fdchunk, rb);
		if(c->step < key) n = n->rb_left;
		else if(c->step > key) n = n->rb_right;
		else {
			/* got it */
			if(ctx != NULL) *ctx = c;
			return c;
		}
	}

	return NULL;
}


static struct fdchunk *insert_chunk_helper(struct fdchunk *c)
{
	struct rb_node **p = &fdchunk_tree.rb_node, *parent = NULL;
	struct fdchunk *oth;
	while(*p != NULL) {
		parent = *p;
		oth = rb_entry(parent, struct fdchunk, rb);
		if(oth->step < c->step) p = &(*p)->rb_left;
		else if(oth->step > c->step) p = &(*p)->rb_right;
		else return oth;
	}
	__rb_link_node(&c->rb, parent, p);
	return NULL;
}


static void insert_chunk(struct fdchunk *c)
{
	struct fdchunk *oth = insert_chunk_helper(c);
	assert(oth == NULL);
	__rb_insert_color(&c->rb, &fdchunk_tree);
}


inline struct fd_bits *__fdbits(void **ctx, int fd) {
	assert(__fd_valid(ctx, fd));
	struct fdchunk *c = get_chunk(ctx, fd);
	assert(c != NULL);
	return ra_id2ptr(ra_fdbits, c->fds[fd % CHUNKSZ].raw >> 1);
}


inline bool __fd_valid(void **ctx, int fd)
{
	if(unlikely(fd < 0)) return false;
	struct fdchunk *c = get_chunk(ctx, fd);
	return unlikely(c == NULL) ? false
		: bitmap_test_bit(c->occ, fd % CHUNKSZ);
}


/* find a free slot for a file descriptor within currently allocated chunks.
 * if all are allocated, return the lowest position where a chunk could be
 * added for a new file descriptor.
 *
 * FIXME: O(n^2) because of no "low FD allocated" stuff, which could be put in
 * at some point. this'd skip some words in the inner loop.
 */
static int first_free_fd(struct fdchunk **c_p, int first)
{
	int gap = -1;	/* discover a gap between chunks */
	RB_FOREACH(rb, &fdchunk_tree) {
		struct fdchunk *c = rb_entry(rb, struct fdchunk, rb);
		if(c->step * CHUNKSZ + CHUNKSZ - 1 < first) continue;
		if(gap < 0 || gap == c->step - 1) gap = c->step;
		int fstoffs = max(0, first - c->step * CHUNKSZ);
		bitmap_word fstmask = ~bitmap_bswap(
			(1ull << (BITMAP_WORD_BITS - fstoffs % BITMAP_WORD_BITS)) - 1);
		for(int i = fstoffs / BITMAP_WORD_BITS;
			i < BITMAP_NWORDS(CHUNKSZ);
			i++)
		{
			bitmap_word w = c->occ[i].w | fstmask;
			fstmask = 0;
			if(w != BITMAP_WORD_1) {
				*c_p = c;
				/* bizarre munging courtesy of CCAN's big-endian bitmaps, for
				 * the benefit of interchangeable memory-mapped data.
				 */
				return c->step * CHUNKSZ + i * BITMAP_WORD_BITS
					+ BITMAP_WORD_BITS - 1 - MSB(bitmap_bswap(~w));
			}
		}
	}

	assert(gap >= 0 || __rb_first(&fdchunk_tree) == NULL
		|| rb_entry(__rb_first(&fdchunk_tree), struct fdchunk, rb)->step * CHUNKSZ + CHUNKSZ - 1 < first);
	*c_p = NULL;
	return unlikely(gap < 0) ? max(0, first) : (gap + 1) * CHUNKSZ;
}


/* @fd can be a negative value to allocate a fd greater or equal to the
 * absolute value, or -1 for "any old". if @fd >= 0, return value is -1
 * [EEXIST] when the file descriptor was already in use, and -1 [ENOMEM] when
 * a chunk wasn't found and couldn't be allocated; *@ctx may be modified.
 */
static int __alloc_fd_ref(void **ctx, int fd, struct fd_bits *bits, int fflags)
{
	assert((fflags & ~1) == 0);
	struct fdchunk *c;
	if(fd >= 0) {
		c = get_chunk(ctx, fd);
		if(c != NULL && bitmap_test_bit(c->occ, fd % CHUNKSZ)) {
			errno = EEXIST;
			return -1;
		}
	} else {
		c = NULL;
		fd = first_free_fd(&c, -fd);
		assert(fd >= 0);
	}
	if(c == NULL) {
		c = calloc(1, sizeof *c);
		if(c == NULL) return -1;
		c->step = fd / CHUNKSZ;
		insert_chunk(c);
	}
	assert(c->step == fd / CHUNKSZ);
	assert(c->step * CHUNKSZ + fd % CHUNKSZ == fd);

	assert(!bitmap_test_bit(c->occ, fd % CHUNKSZ));
	bitmap_set_bit(c->occ, fd % CHUNKSZ);
	c->fds[fd % CHUNKSZ].raw = ra_ptr2id(ra_fdbits, bits) << 1 | fflags;
	bits->refs++;

	assert(__fd_valid(ctx, fd));
	return fd;
}


int __alloc_fd_bits(
	void **ctx, int fd,
	L4_ThreadId_t server, L4_Word_t handle, int fflags)
{
	struct fd_bits *bits = ra_alloc(ra_fdbits, -1);
	*bits = (struct fd_bits){ .server = server, .handle = handle };
	int ret = __alloc_fd_ref(ctx, fd, bits, fflags);
	if(unlikely(ret < 0)) ra_free(ra_fdbits, bits);
	assert(bits->refs > 0);
	return ret;
}


int __fd_first(struct fd_iter *it)
{
	struct rb_node *low = __rb_first(&fdchunk_tree);
	if(low == NULL) {
		it->chunk = NULL;
		return -1;
	}

	it->chunk = rb_entry(low, struct fdchunk, rb);
	return __fd_next(it, -1);
}


int __fd_next(struct fd_iter *it, int prev)
{
	int bit = bitmap_ffs(it->chunk->occ,
		max_t(int, 0, prev - it->chunk->step * CHUNKSZ + 1), CHUNKSZ);
	if(bit < CHUNKSZ) {
		return it->chunk->step * CHUNKSZ + bit;
	} else {
		/* continue from next chunk over, or complete. */
		it->chunk = container_of_or_null(__rb_next(&it->chunk->rb),
			struct fdchunk, rb);
		return it->chunk == NULL ? -1 : __fd_next(it, -1);
	}
}


/* this isn't as much cold as init-only. */
COLD void __file_init(struct sneks_fdlist *fdlist)
{
	ra_fdbits = RA_NEW(struct fd_bits, 1 << 15);

	if(fdlist == NULL || fdlist->next == 0) return;

	void *ctx = NULL;
	int prev = fdlist->fd, n;
	do {
		if(fdlist->fd > prev) abort();	/* invalid fdlist */
		prev = fdlist->fd;
		n = __alloc_fd_bits(&ctx, fdlist->fd, fdlist->serv, fdlist->cookie, 0);
		fdlist = sneks_fdlist_next(fdlist);
	} while(n >= 0 && fdlist->next != 0);
	int err = errno;

	stdin = fdopen(0, "r");
	stdout = fdopen(1, "w");
	stderr = fdopen(2, "w");

	if(n < 0) {
		fprintf(stderr, "%s: __alloc_fd_bits() failed, errno=%d\n",
			__func__, err);
		abort();
	}
}


int close(int fd)
{
	void *ctx = NULL;
	if(!__fd_valid(&ctx, fd)) {
		errno = EBADF;
		return -1;
	}
	struct fdchunk *c = get_chunk(&ctx, fd);
	assert(bitmap_test_bit(c->occ, fd % CHUNKSZ));
	struct fd_bits *b = ra_id2ptr(ra_fdbits, c->fds[fd % CHUNKSZ].raw >> 1);
	assert(b->refs > 0);
	bitmap_clear_bit(c->occ, fd % CHUNKSZ);
	assert(!__fd_valid(&ctx, fd));
	if(--b->refs > 0) return 0;
	else {
		int n = __io_close(b->server, b->handle);
		ra_free(ra_fdbits, b);
		return NTOERR(n);
	}
}


long read(int fd, void *buf, size_t count)
{
	void *ctx = NULL;
	if(!__fd_valid(&ctx, fd)) {
		errno = EBADF;
		return -1;
	}
	if(count == 0) return 0;

	int n;
	unsigned length;
	struct fd_bits *b = __fdbits(&ctx, fd);
	__permit_recv_interrupt();
	do {
		length = count;
		n = __io_read(b->server, b->handle, count, buf, &length);
	} while(n == -EAGAIN);
	__forbid_recv_interrupt();
	if(n == -EWOULDBLOCK) n = -EAGAIN;
	return NTOERR(n, length);
}


long write(int fd, const void *buf, size_t count)
{
	void *ctx = NULL;
	if(!__fd_valid(&ctx, fd)) {
		errno = EBADF;
		return -1;
	}
	if(count == 0) return 0;

	uint16_t rc;
	int n;
	struct fd_bits *b = __fdbits(&ctx, fd);
	do {
		n = __io_write(b->server, &rc, b->handle, buf, count);
	} while(n == -EAGAIN);
	if(n == -EWOULDBLOCK) n = -EAGAIN;
	return NTOERR(n, rc);
}


uint64_t lseek(int fd, uint64_t offset, int whence)
{
	errno = ENOSYS;
	return -1;
}


int dup(int oldfd) {
	return dup2(oldfd, -3);
}


/* FIXME: make this signal-safe */
int dup2(int oldfd, int newfd)
{
	void *ctx = NULL;
	if(!__fd_valid(&ctx, oldfd)) {
		errno = EBADF;
		return -1;
	}
	if(oldfd == newfd) return oldfd;	/* no-op */
	if(newfd >= 0 && __fd_valid(&ctx, newfd)) close(newfd);
	return __alloc_fd_ref(&ctx, newfd, __fdbits(&ctx, oldfd), 0);
}


int dup3(int oldfd, int newfd, int flags)
{
	if(oldfd == newfd) {
		errno = EINVAL;
		return -1;
	}

	errno = ENOSYS;
	return -1;
}


int fcntl(int fd, int cmd, ...)
{
	void *ctx = NULL;
	if(!__fd_valid(&ctx, fd)) {
		errno = EBADF;
		return -1;
	}
	struct fd_bits *b = __fdbits(&ctx, fd);
	va_list al; va_start(al, cmd);
	switch(cmd) {
		default: va_end(al); goto Einval;
		case F_DUPFD: {
			int low = va_arg(al, int);
			va_end(al);
			if(low < 0) goto Einval;
			if(low == 0 && __fd_valid(&ctx, 0)) low = 1;
			return __alloc_fd_ref(&ctx, -low, b, 0);
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
			int foo, n = __io_set_flags(b->server, &foo, b->handle, val, val);
			return NTOERR(n);
		}
	}

	errno = ENOSYS;
	return -1;

Einval:
	errno = EINVAL;
	return -1;
}
