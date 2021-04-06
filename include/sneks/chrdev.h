
/* interface of libsneks-chrdev.a .
 *
 * the library calls its client's designated callback functions to implement
 * behaviour of character device files while relying on sys/io for basic IO
 * handle functionality.
 */

#ifndef _SNEKS_CHRDEV_H
#define _SNEKS_CHRDEV_H

#include <stddef.h>

#include <sneks/io.h>


/* this will be defined by the client, its size specified in chrdev_run(), and
 * used to prevent void pointer fuckage in the callback prototypes.
 */
#define chrdev_file_impl io_file_impl
typedef struct chrdev_file_impl chrfile_t;


/* @mask is a set of EPOLL{IN,OUT,ERR,HUP}, and @file identifies the endpoint
 * wrt which @mask is read by userspace. the library sends wakeups, poll
 * replies, or nothing at all accordingly.
 */
#define chrdev_notify(file, mask) io_notify((file), (mask))

/* callback control. these take effect when the next event occurs after the
 * control function has returned.
 *
 * the read and write callbacks may return -EWOULDBLOCK on buffer empty and
 * full, respectively. wakeups are performed through chrdev_notify() at buffer
 * state change, and the library handles O_NONBLOCK, polling, and blocking
 * behind the scenes.
 */
#define chrdev_get_status_func(fn) io_get_status_func((fn))
#define chrdev_read_func(fn) io_read_func((fn))
#define chrdev_write_func(fn) io_write_func((fn))
#define chrdev_confirm_func(fn) io_confirm_func((fn))
extern void chrdev_close_func(int (*fn)(chrfile_t *));
#define chrdev_ioctl_func(fn) io_ioctl_func((fn))

/* TODO: wrap io_fast_confirm_flags(), io_set_fast_confirm(). for the time
 * being implementors can use those directly.
 */


/* maps to Sneks::Pipe/pipe, for creation of a pipe buffer and its two
 * endpoints associated with the same client process.
 */
extern void chrdev_pipe_func(
	int (*fn)(chrfile_t *readside, chrfile_t *writeside, int flags));

/* maps to Sneks::DeviceNode/open, for creation of device handles. libchrdev
 * decodes the "object" parameter into type, major, minor. @type is 'c' for
 * character devices and 'b' for block devices; the callback should return
 * -ENODEV for any combination of type-major-minor, or set flag in @flags, it
 * doesn't recognize.
 */
extern void chrdev_dev_open_func(
	int (*fn)(chrfile_t *h, char type, int major, int minor, int flags));

/* returns like main() */
extern int chrdev_run(size_t sizeof_chrdev_file, int argc, char *argv[]);

#endif
