/* interface of sys/chrdev. */
#ifndef _SNEKS_CHRDEV_H
#define _SNEKS_CHRDEV_H

#include <stddef.h>
#include <sneks/io.h>

#define chrdev_file_impl io_file_impl
typedef struct chrdev_file_impl chrfile_t;
extern int chrdev_run(size_t sizeof_chrdev_file, int argc, char *argv[]);

/* notify userspace about buffer status change according to @mask on @file.
 * @mask is a set of EPOLL{IN,OUT,ERR,HUP}. @file identifies the endpoint wrt
 * which @mask is read by userspace. the library sends wakeups, poll replies,
 * or nothing at all accordingly.
 */
#define chrdev_notify(file, mask) io_notify((file), (mask))

/* callback control takes effect when the next event occurs after the control
 * function has returned.
 *
 * read and write callbacks may return -EWOULDBLOCK on buffer empty and full,
 * respectively. wakeups are performed through chrdev_notify() at buffer state
 * change. library handles O_NONBLOCK, polling, and blocking under the hood.
 */
#define chrdev_get_status_func(fn) io_get_status_func((fn))
#define chrdev_read_func(fn) io_read_func((fn))
#define chrdev_write_func(fn) io_write_func((fn))
#define chrdev_confirm_func(fn) io_confirm_func((fn))
extern void chrdev_close_func(int (*fn)(chrfile_t *));
#define chrdev_ioctl_func(fn) io_ioctl_func((fn))
#define chrdev_stat_func(fn) io_stat_func((fn))

/* maps to Sneks::Pipe/pipe. creation of a pipe buffer and its two endpoints
 * associated with the same client process.
 */
extern void chrdev_pipe_func(int (*fn)(chrfile_t *readside, chrfile_t *writeside, int flags));

/* maps to Sneks::DeviceNode/open. creation of device handles. libchrdev
 * decodes the "object" parameter into type, major, minor; @type is 'c' for
 * character devices and 'b' for block devices. callback returns -ENODEV for
 * any combination of type-major-minor, or set flag in @flags, it doesn't
 * recognize.
 */
extern void chrdev_dev_open_func(int (*fn)(chrfile_t *h, char type, int major, int minor, int flags));

#endif
