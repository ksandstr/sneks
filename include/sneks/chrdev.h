
/* interface of libsneks-chrdev.a */

#ifndef _SNEKS_CHRDEV_H
#define _SNEKS_CHRDEV_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/epoll.h>


/* this will be defined by the client, its size specified in chrdev_run(), and
 * used to prevent void pointer fuckage in the callback prototypes.
 */
struct chrdev_file;
typedef struct chrdev_file chrfile_t;


/* @mask is a set of EPOLL{IN,OUT,ERR,HUP}, and @file identifies the endpoint
 * wrt which @mask is read by userspace. the library sends wakeups, poll
 * replies, or nothing at all accordingly.
 */
extern void chrdev_notify(chrfile_t *file, int mask);

/* callback control. these take effect when the next event occurs after the
 * control function has returned.
 *
 * the read and write callbacks may return -EWOULDBLOCK on buffer empty and
 * full, respectively. wakeups are performed through chrdev_notify() at buffer
 * state change, and the library handles O_NONBLOCK, polling, and blocking
 * behind the scenes.
 *
 * note on typing: where POSIX read(2) and write(2) take a size_t count and
 * return ssize_t, the underlying microkernel mechanism is limited to
 * significantly shorter block transfers than "half the address space", so the
 * native C types are used here instead. also, POSIX ioctl's @request is an
 * int but GNU/Linux specifies unsigned long; and there doesn't seem to be an
 * use case for adding and subtracting these so we'll go with the latter.
 */
extern void chrdev_get_status_func(int (*fn)(chrfile_t *file));
extern void chrdev_dead_client_func(int (*fn)(pid_t pid));
extern void chrdev_read_func(
	int (*fn)(chrfile_t *file, uint8_t *buf, unsigned count));
extern void chrdev_write_func(
	int (*fn)(chrfile_t *file, const uint8_t *buf, unsigned count));
extern void chrdev_confirm_func(
	void (*fn)(chrfile_t *file, unsigned count, bool));
extern void chrdev_close_func(int (*fn)(chrfile_t *file));
extern void chrdev_ioctl_func(
	int (*fn)(chrfile_t *file, unsigned long request, va_list args));

/* callback is invoked for each handle to be copied when a client forks. */
extern void chrdev_fork_func(int (*fn)(chrfile_t *dst, chrfile_t *src));

/* maps to Sneks::Pipe/pipe, for creation of a pipe buffer and its two
 * endpoints associated with the same client process.
 */
extern void chrdev_pipe_func(
	int (*fn)(chrfile_t *readside, chrfile_t *writeside, int flags));

/* returns like main() */
extern int chrdev_run(size_t sizeof_chrdev_file, int argc, char *argv[]);

#endif
