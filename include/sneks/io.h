/* interface of sys/io. */
#ifndef _SNEKS_IO_H
#define _SNEKS_IO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/typesafe_cb/typesafe_cb.h>

struct io_file_impl;
typedef struct io_file_impl iof_t;
extern int io_run(size_t iof_size, int argc, char *argv[]);

enum lifecycle_tag {
	CLIENT_FORK = 1,
	CLIENT_EXIT = 2,
	CLIENT_EXEC = 3,
};

/* file status flags IOF_*. */
#define IOF_NONBLOCK SNEKS_IO_O_NONBLOCK
#define IOF_VALID_MASK (IOF_NONBLOCK)

/* file descriptor flags IOD_* at bit 16 in fd->flags. */
#define IOD_CLOEXEC (SNEKS_IO_FD_CLOEXEC << 16)
#define IOD_VALID_MASK (IOD_CLOEXEC)

/* fast confirm flags IO_CONFIRM_*. */
#define IO_CONFIRM_READ		0x1
#define IO_CONFIRM_WRITE	0x2
#define IO_CONFIRM_CLOSE	0x4
#define IO_CONFIRM_VALID_MASK 0x7

/* called from a callback to cause io_run() to return @rc. returns negative
 * errno on failure, 0 on success.
 */
extern int io_quit(int rc);

/* file and handle creation.
 *
 * an implementor calls iof_new() to create a new file such as for open(2),
 * pipe(2), and socket(2). if malloc() fails or @flags has reserved bits set,
 * NULL is returned. @flags is a set of file status flags IOF_*.
 *
 * if the subsequent creating operation fails or is rolled back, the
 * implementor should call iof_undo_new() on file descriptions already
 * created. like free(3) it is safe against @file=NULL and does not invoke
 * the io_close_func() callback.
 */
extern iof_t *iof_new(int flags);
extern void iof_undo_new(iof_t *file);

/* io_add_fd() creates a handle associating @file with @pid and returns a
 * non-negative integer, or fails and returns a negative POSIX errno.
 * @flags is a set of handle flags IOD_*; if any reserved bits are set,
 * -EINVAL is returned.
 */
extern int io_add_fd(pid_t pid, iof_t *file, int flags);

/* io_get_file() resolves the handle @fd to a file for @pid. a NULL result may
 * be passed as EBADF. as a special case, when @pid == -1 it'll disregard @fd
 * and return an arbitrary iof_t or NULL if there are no active descriptors
 * for any client.
 *
 * note that this function should never be called from within a
 * confirm/rollback handler or from the io_lifecycle_func() callback. in the
 * former case it'll break when lifecycle events invalidate @pid and/or @fd,
 * and in the latter it leads to recursion most foul. instead of doing that,
 * reference <iof_t *> directly and rely on confirm/rollback running before
 * lifecycle processing.
 */
extern iof_t *io_get_file(pid_t pid, int fd);

/* callback control. these take effect when the next IPC dispatch occurs after
 * the control function has returned.
 */

/* client lifecycle callbacks.
 *
 * parameters are passed in the varargs portion according to @tag.
 *   - CLIENT_EXIT, @client has exited. no parameters.
 *   - CLIENT_FORK, @client has forked. parameter is pid_t child_pid.
 *   - CLIENT_EXEC, @client has replaced its process image using an execve()
 *     family function. no parameters.
 *
 * this callback runs synchronously regardless of multithread concerns such
 * that its effects have completed before any further operations run.
 *
 * sys/io processes its own lifecycle handling before entering this callback,
 * so e.g. a close_func that destroys an auxiliary context of @client should
 * be accounted for regardless of @tag value.
 */
extern void io_lifecycle_func(void (*fn)(pid_t client, enum lifecycle_tag tag, ...));

/* callbacks for operations of Sneks::IO.
 *
 * the close callback is invoked lazily when the IPC reply has been confirmed
 * successful. if the callback has external side effects such as hangup
 * notifications, set IO_CONFIRM_CLOSE to ensure it's run eagerly.
 */
extern void io_close_func(int (*fn)(iof_t *file));

/* read and write should return the number of bytes read or written, or a
 * negative errno status as read(2) and write(2). -EWOULDBLOCK is handled by
 * the library by raising Posix::Errno or muidl::NoReply according to whether
 * O_NONBLOCK is set on @file. implementors that return -EWOULDBLOCK should
 * also use the Sneks::Poll part of sys/io and call io_notify() when the
 * corresponding un-blocking event occurs.
 *
 * note on typing: where POSIX read(2) and write(2) take a size_t count and
 * return ssize_t, the underlying microkernel mechanism is limited to
 * significantly shorter block transfers than half the 32-bit address space so
 * native C types are used instead.
 */
extern void io_read_func(int (*fn)(iof_t *file, uint8_t *buf, unsigned count, off_t offset));
extern void io_write_func(int (*fn)(iof_t *file, const uint8_t *buf, unsigned count, off_t offset));

/* POSIX ioctl's @request is an int but GNU/Linux specifies unsigned long, and
 * there doesn't seem to be an use case for adding and subtracting an
 * operation tag, so we'll go long for sign compatibility with the former and
 * bit-length compatibility with the latter.
 */
extern void io_ioctl_func(int (*fn)(iof_t *file, long request, va_list args));

/* I/O confirmation callback.
 *
 * if set and not NULL after a read/write callback returns, sys/io sets the
 * per-thread confirm callback in io_impl_{read,write}() to one that fetches
 * the iof_t involved and passes the byte count returned by read/write, and
 * the offset parameter of Sneks::IO/{read,write}, to @fn. @writing indicates
 * which of those is being confirmed.
 *
 * if not set or set to NULL, sys/io leaves confirm/rollback callbacks
 * entirely alone.
 */
extern void io_confirm_func(void (*fn)(iof_t *file, unsigned count, off_t offset, bool writing));

/* set the fast confirm flags, IO_CONFIRM_*. defaults to all clear.
 *
 * for each set flag, when the corresponding IDL call would succeed the
 * confirm callback is invoked without delay even if the next incoming IDL
 * transaction occurs later. this allows e.g. pipe hangups to be signaled
 * right away rather than when the next IPC message is received.
 */
extern void io_fast_confirm_flags(int flags);

/* cause eager invocation of the confirm callback after the current IDL
 * callback returns. this allows fast confirms at a finer than per-callback
 * grain and in IDL-mediated operations outside the sys/io domain (such as
 * certain ioctls).
 */
extern void io_set_fast_confirm(void);

/* interface of the optional Sneks::Poll module.
 *
 * use of these symbols causes linking of code that filesystems and block
 * devices don't need.
 */

/* file status query. @fn returns a mask of EPOLL{IN,PRI,OUT,ERR,HUP,RDHUP}.
 * must be specified if FILL_SNEKS_POLL() is used.
 */
extern void io_get_status_func(int (*fn)(iof_t *file));

/* edge-triggered I/O status notification on that same set of events in
 * @epoll_mask. EPOLLIN and EPOLLHUP also wake up blocked readers, and
 * EPOLLOUT wakes up blocked writers.
 */
extern void io_notify(iof_t *file, int epoll_mask);

/* non-portable parts below. */
#if defined(__sneks__) || defined(__SNEKS__)
#include <l4/types.h>
#include <sneks/api/io-defs.h>

/* vtable alteration. FILL_SNEKS_IO() sets all vtable entries for Sneks::IO in
 * @vtab to the corresponding io_impl_{name}() routines.
 */
#define FILL_SNEKS_IO(vtab) do { \
		(vtab)->set_file_flags = &io_impl_set_file_flags; \
		(vtab)->set_handle_flags = &io_impl_set_handle_flags; \
		(vtab)->write = &io_impl_write; \
		(vtab)->read = &io_impl_read; \
		(vtab)->close = &io_impl_close; \
		(vtab)->dup = &io_impl_dup; \
		(vtab)->dup_to = &io_impl_dup_to; \
		(vtab)->touch = &io_impl_touch; \
		(vtab)->isatty = &io_impl_isatty; \
	} while(false)

/* dispatcher function. should return what its inner
 * _muidl_sneks_io_dispatch(), or derivative, call does.
 */
#define io_dispatch_func(fn, priv) \
	_io_dispatch_func(typesafe_cb_cast3(L4_Word_t (*)(void *), \
			L4_Word_t (*)(typeof(*(priv)) *), \
			L4_Word_t (*)(const typeof(*(priv)) *), \
			L4_Word_t (*)(const void *), (fn)), \
		(priv))
extern void _io_dispatch_func(L4_Word_t (*fn)(void *priv), const void *priv);

/* implementations of Sneks::IO. these are either used in the implementor's
 * IDL vtable or wrapped through something else.
 */
extern int io_impl_set_file_flags(int *, int, int, int);
extern int io_impl_set_handle_flags(int *, int, int, int);
extern int io_impl_write(int, off_t, const uint8_t *, unsigned);
extern int io_impl_read(int, int, off_t, uint8_t *, unsigned *);
extern int io_impl_close(int);
extern int io_impl_dup(int *, int, int);
extern int io_impl_dup_to(int *, int, pid_t);
extern int io_impl_touch(int);
extern int io_impl_isatty(int);

/* same for Sneks::Poll. */
#define FILL_SNEKS_POLL(vtab) do { \
		(vtab)->set_notify = &io_impl_set_notify; \
		(vtab)->get_status = &io_impl_get_status; \
	} while(false)

extern int io_impl_set_notify(int *, int, int, L4_Word_t);
extern void io_impl_get_status(const int *, unsigned, const uint16_t *, unsigned, int *, unsigned *);

#endif

#endif
