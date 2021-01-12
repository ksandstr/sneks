
/* TODO:
 *   - PIPE_BUF atomicity
 *   - ioctl FIONREAD (non-POSIX)
 *   - fcntl F_GETPIPE_SZ, F_SETPIPE_SZ (non-POSIX)
 *   - O_ASYNC (in sys/chrdev)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>
#include <ccan/membuf/membuf.h>

#include <sneks/chrdev.h>


/* due to the way CCAN membuf works, it may be the case that pipes appear full
 * with only half of PIPESZ available to read. this should still be enough,
 * and there'll usually be room for more.
 */
#define PIPESZ 4096

#define PHF_WRITER 1	/* if not reader */

/* NOTE: IS_FULL() may be invalid until membuf_prepare_space(&(hd)->buf, 1)
 * has run.
 */
#define IS_FULL(hd) (membuf_num_space(&(hd)->buf) == 0)
#define IS_EMPTY(hd) (membuf_num_elems(&(hd)->buf) == 0)


struct pipehead
{
	/* NOTE: ideally we'd have a more cleverer VM ringbuffer here, and
	 * libsneks-chrdev.a's interface would let strxfer right out of said
	 * buffer without an extra memcpy; but that can wait until tests become
	 * good enough to trust something so fancy.
	 */
	MEMBUF(char) buf;
	struct list_head readh, writeh;	/* of <struct chrdev_file> */
};


struct chrdev_file
{
	/* link in ->head's readh or writeh, per PHF_WRITER in ->flags. */
	struct list_node ph_link;
	struct pipehead *head;
	int flags;	/* PHF_* */
};


static void wake(struct list_head *list, int mask)
{
	chrfile_t *cur;
	list_for_each(list, cur, ph_link) {
		chrdev_notify(cur, mask);
	}
}


static void pipe_file_dtor(struct chrdev_file *h)
{
	/* disconnect from pipe head. */
	struct list_head *hh = h->flags & PHF_WRITER
		? &h->head->writeh : &h->head->readh;
	list_del_from(hh, &h->ph_link);

	/* test for pipe head destruction, or send EOF to waiting readers when the
	 * last writer is removed.
	 */
	if(list_empty(&h->head->writeh) && list_empty(&h->head->readh)) {
		free(membuf_cleanup(&h->head->buf));
		free(h->head);
	} else if(list_empty(&h->head->writeh)) {
		wake(&h->head->readh, EPOLLHUP);
	}
}


static void add_handle(struct pipehead *hd, struct chrdev_file *handle) {
	assert(handle->head == hd);
	list_add_tail(handle->flags & PHF_WRITER ? &hd->writeh : &hd->readh,
		&handle->ph_link);
}


static int pipe_fork(chrfile_t *copy, chrfile_t *parent)
{
	*copy = *parent;
	add_handle(parent->head, copy);
	return 0;
}


static void *membuf_snub(struct membuf *mb, void *rawptr, size_t newsize) {
	/* let's not resize pipes all willy-nilly. */
	return NULL;
}


static int pipe_pipe(chrfile_t *readh, chrfile_t *writeh, int flags)
{
	if(flags != 0) return -EINVAL;

	struct pipehead *hd = malloc(sizeof *hd);
	if(hd == NULL) return -ENOMEM;
	char *firstbuf = aligned_alloc(4096, PIPESZ);
	if(firstbuf == NULL) {
		free(hd);
		return -ENOMEM;
	}

	membuf_init(&hd->buf, firstbuf, PIPESZ, &membuf_snub);
	list_head_init(&hd->readh);
	list_head_init(&hd->writeh);
	*readh = (struct chrdev_file){ .head = hd };
	add_handle(hd, readh);
	*writeh = (struct chrdev_file){ .head = hd, .flags = PHF_WRITER };
	add_handle(hd, writeh);

	return 0;
}


static int pipe_close(chrfile_t *h) {
	pipe_file_dtor(h);
	return 0;
}


static int pipe_get_status(chrfile_t *h)
{
	uint32_t st;
	if(h->flags & PHF_WRITER) {
		if(list_empty(&h->head->readh)) st = EPOLLERR;	/* EPIPE pending */
		else {
			membuf_prepare_space(&h->head->buf, 1);
			if(!IS_FULL(h->head)) st = EPOLLOUT;
			else st = 0;
		}
	} else {
		if(!IS_EMPTY(h->head)) st = EPOLLIN;
		else if(list_empty(&h->head->writeh)) st = EPOLLHUP;
		else st = 0;
	}
	return st;
}


static int pipe_write(chrfile_t *h, const uint8_t *buf, unsigned buf_len)
{
	if(list_empty(&h->head->readh)) return -EPIPE;	/* bork'd */
	if(buf_len == 0) return 0;
	membuf_prepare_space(&h->head->buf, buf_len);
	bool was_empty = IS_EMPTY(h->head);
	if(IS_FULL(h->head)) return -EWOULDBLOCK;

	size_t written = min(buf_len, membuf_num_space(&h->head->buf));
	memcpy(membuf_space(&h->head->buf), buf, written);
	if(was_empty) wake(&h->head->readh, EPOLLIN);

	return written;
}


static int pipe_read(chrfile_t *h, uint8_t *buf, unsigned count)
{
	if(IS_EMPTY(h->head)) {
		if(!list_empty(&h->head->writeh)) return -EWOULDBLOCK;
		else {
			/* send EOF only when all writers have closed and the buffer is
			 * empty.
			 */
			return 0;
		}
	}

	membuf_prepare_space(&h->head->buf, 1);
	bool was_full = IS_FULL(h->head);
	size_t got = min_t(size_t, count, membuf_num_elems(&h->head->buf));
	memcpy(buf, membuf_elems(&h->head->buf), got);
	if(got > 0 && was_full) wake(&h->head->writeh, EPOLLOUT);

	return got;
}


static void pipe_confirm(chrfile_t *h, unsigned count, bool writing)
{
	/* for pipes we'll use @writing to double-check that things are coming
	 * down the right way, even as there's no reason to expect they wouldn't.
	 * bidirectional socket servers and the like should just trust the
	 * framework.
	 */
	if(writing != !!(h->flags & PHF_WRITER)) {
		/* and even here, the best we can do is warn a brotha. really we
		 * should invalidate the file descriptor so that future access always
		 * pops errors, seeing as we've come to an indeterminate state. but
		 * screw that, the framework's idea is always correct enough.
		 */
		fprintf(stderr, "%s: @writing doesn't match PHF_WRITER‽\n", __func__);
	}

	if(h->flags & PHF_WRITER) {
		membuf_added(&h->head->buf, count);
	} else {
		membuf_consume(&h->head->buf, count);
	}
}


static int pipe_ioctl(chrfile_t *h, unsigned long request, va_list args)
{
	/* for now, we recognize nothing.
	 *
	 * TODO: we should recognize things at some point as the ioctl
	 * responsibilities shake out.
	 */
	return -EINVAL;
}


int main(int argc, char *argv[])
{
	chrdev_get_status_func(&pipe_get_status);
	chrdev_read_func(&pipe_read);
	chrdev_write_func(&pipe_write);
	chrdev_confirm_func(&pipe_confirm);
	chrdev_close_func(&pipe_close);
	chrdev_ioctl_func(&pipe_ioctl);
	chrdev_fork_func(&pipe_fork);
	chrdev_pipe_func(&pipe_pipe);

	return chrdev_run(sizeof(struct chrdev_file), argc, argv);
}
