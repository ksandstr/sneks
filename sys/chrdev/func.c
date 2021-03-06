
/* callback wrangling per <sneks/chrdev.h> */

#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include "private.h"


static int enosys();	/* works for 3 or fewer args? wtf. */
static int no_device(chrfile_t *, char, int, int, int);
static void no_confirm(chrfile_t *, unsigned, bool);


struct chrdev_callbacks callbacks = {
	.get_status = &enosys,
	.dead_client = &enosys,
	.read = &enosys, .write = &enosys,
	.confirm = &no_confirm,
	.close = &enosys,
	.ioctl = &enosys,
	.pipe = &enosys,
	.fork = &enosys,
	.dev_open = &no_device,
};


static int enosys() { return -ENOSYS; }
static int no_device(chrfile_t *a, char b, int c, int d, int e) {
	return -ENODEV;
}


static void no_confirm(chrfile_t *foo, unsigned bar, bool zot) {
	/* does nothing */
}


void chrdev_get_status_func(int (*fn)(chrfile_t *)) {
	callbacks.get_status = fn;
}


void chrdev_dead_client_func(int (*fn)(pid_t)) {
	callbacks.dead_client = fn;
}


void chrdev_read_func(int (*fn)(chrfile_t *, uint8_t *, unsigned)) {
	callbacks.read = fn;
}


void chrdev_write_func(int (*fn)(chrfile_t *, const uint8_t *, unsigned)) {
	callbacks.write = fn;
}


void chrdev_confirm_func(void (*fn)(chrfile_t *, unsigned, bool)) {
	callbacks.confirm = fn;
}


void chrdev_close_func(int (*fn)(chrfile_t *)) {
	callbacks.close = fn;
}


void chrdev_ioctl_func(int (*fn)(chrfile_t *, unsigned long, va_list)) {
	callbacks.ioctl = fn;
}


void chrdev_fork_func(int (*fn)(chrfile_t *, chrfile_t *)) {
	callbacks.fork = fn;
}


void chrdev_pipe_func(int (*fn)(chrfile_t *, chrfile_t *, int)) {
	callbacks.pipe = fn;
}


void chrdev_dev_open_func(int (*fn)(chrfile_t *h, char, int, int, int)) {
	callbacks.dev_open = fn;
}
