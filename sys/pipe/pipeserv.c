/* TODO:
 *   - PIPE_BUF atomicity
 *   - ioctl FIONREAD (non-POSIX)
 *   - fcntl F_GETPIPE_SZ, F_SETPIPE_SZ (non-POSIX)
 *   - O_ASYNC (in sys/chrdev)
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/minmax/minmax.h>
#include <ccan/membuf/membuf.h>
#include <sneks/systask.h>
#include <sneks/chrdev.h>

/* because of how CCAN membuf works, pipes may appear full with only half of
 * PIPESZ available to read. this value should be large enough.
 */
#define PIPESZ 4096

#define WRITER 1 /* if not reader */

/* NOTE: IS_FULL() is lazy until membuf_prepare_space(&(hd)->buf, 1). */
#define IS_FULL(hd) (membuf_num_space(&(hd)->buf) == 0)
#define IS_EMPTY(hd) (membuf_num_elems(&(hd)->buf) == 0)

struct pipehead {
	MEMBUF(char) buf;
	chrfile_t *readf, *writef;
};

struct chrdev_file_impl {
	struct pipehead *head;
	int flags;
};

static void *membuf_snub(struct membuf *mb, void *rawptr, size_t newsize) {
	/* let's not resize pipes just because. */
	return NULL;
}

static int pipe_pipe(chrfile_t *readf, chrfile_t *writef, int flags)
{
	struct pipehead *hd = malloc(sizeof *hd);
	char *firstbuf = aligned_alloc(4096, PIPESZ);
	if(hd == NULL || firstbuf == NULL) { free(hd); free(firstbuf); return -ENOMEM; }
	membuf_init(&hd->buf, firstbuf, PIPESZ, &membuf_snub);
	hd->readf = readf; hd->writef = writef;
	*readf = (chrfile_t){ .head = hd };
	*writef = (chrfile_t){ .head = hd, .flags = WRITER };
	return 0;
}

static int pipe_close(chrfile_t *f)
{
	if(f->flags & WRITER) {
		assert(f->head->writef == f);
		f->head->writef = NULL;
	} else {
		assert(f->head->readf == f);
		f->head->readf = NULL;
	}
	if(f->head->writef == NULL && f->head->readf == NULL) {
		free(membuf_cleanup(&f->head->buf));
		free(f->head);
	} else if(f->head->writef == NULL) {
		/* signal waiting readers upon removal of last writer */
		chrdev_notify(f->head->readf, EPOLLHUP);
	}
	return 0;
}

static int pipe_get_status(chrfile_t *f)
{
	int st;
	if(f->flags & WRITER) {
		if(f->head->readf == NULL) st = EPOLLERR; /* EPIPE pending */
		else {
			membuf_prepare_space(&f->head->buf, 1);
			st = IS_FULL(f->head) ? 0 : EPOLLOUT;
		}
	} else {
		if(!IS_EMPTY(f->head)) st = EPOLLIN;
		else if(f->head->writef == NULL) st = EPOLLHUP;
		else st = 0;
	}
	return st;
}

static int pipe_write(chrfile_t *f, const uint8_t *buf, unsigned buf_len, off_t offset)
{
	if(offset >= 0) return -ESPIPE;
	if(f->head->readf == NULL) return -EPIPE; /* bork'd */
	if(buf_len == 0) return 0;
	membuf_prepare_space(&f->head->buf, buf_len);
	bool was_empty = IS_EMPTY(f->head);
	if(IS_FULL(f->head)) return -EWOULDBLOCK;
	size_t written = min(buf_len, membuf_num_space(&f->head->buf));
	memcpy(membuf_space(&f->head->buf), buf, written);
	if(was_empty) {
		/* TODO: see comment in pipe_read(). */
		chrdev_notify(f->head->readf, EPOLLIN);
	}
	return written;
}

static int pipe_read(chrfile_t *f, uint8_t *buf, unsigned count, off_t offset)
{
	if(offset >= 0) return -ESPIPE;
	if(IS_EMPTY(f->head)) {
		if(f->head->writef != NULL) return -EWOULDBLOCK;
		else {
			/* send EOF only when all writers have closed and the buffer is
			 * empty.
			 */
			return 0;
		}
	}
	membuf_prepare_space(&f->head->buf, 1);
	bool was_full = IS_FULL(f->head);
	size_t got = min_t(size_t, count, membuf_num_elems(&f->head->buf));
	memcpy(buf, membuf_elems(&f->head->buf), got);
	if(f->head->writef != NULL && got > 0 && was_full) {
		/* TODO: this sends spurious notifications when the read-reply fails.
		 * that should be hit in a test case, and this part moved into
		 * pipe_confirm() and replaced with a call to io_set_fast_confirm().
		 */
		chrdev_notify(f->head->writef, EPOLLOUT);
	}
	return got;
}

static void pipe_confirm(chrfile_t *f, unsigned count, off_t offset, bool writing)
{
	assert(offset < 0);
	/* for pipes we'll use @writing to double-check that things are coming
	 * down the right way, even as there's no reason to expect they wouldn't.
	 * bidirectional socket servers and the like should just trust the
	 * framework.
	 */
	if(writing != !!(f->flags & WRITER)) {
		/* and even here, the best we can do is warn a brotha. really we
		 * should invalidate the file descriptor so that future access always
		 * pops errors, seeing as we've come to an indeterminate state. but
		 * screw that, the framework's idea is always correct enough.
		 */
		log_crit("@writing doesn't match WRITER");
		abort();
	}
	if(f->flags & WRITER) {
		membuf_added(&f->head->buf, count);
	} else {
		membuf_consume(&f->head->buf, count);
	}
}

static int pipe_ioctl(chrfile_t *f, long request, va_list args) {
	return -EINVAL; /* fuck you, i won't do what you tell me */
}

int main(int argc, char *argv[])
{
	io_fast_confirm_flags(IO_CONFIRM_CLOSE);
	chrdev_get_status_func(&pipe_get_status);
	chrdev_read_func(&pipe_read);
	chrdev_write_func(&pipe_write);
	chrdev_confirm_func(&pipe_confirm);
	chrdev_close_func(&pipe_close);
	chrdev_ioctl_func(&pipe_ioctl);
	chrdev_pipe_func(&pipe_pipe);
	return chrdev_run(sizeof(struct chrdev_file_impl), argc, argv);
}
