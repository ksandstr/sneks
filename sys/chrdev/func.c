
/* callback wrangling per <sneks/chrdev.h> */

#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include <sneks/chrdev.h>

#include "private.h"


static int enosys();
static int no_device(chrfile_t *, char, int, int, int);


struct chrdev_callbacks chrdev_callbacks = {
	.pipe = &enosys,
	.dev_open = &no_device,
	.close = &enosys,
};


static int enosys() { return -ENOSYS; }
static int no_device(chrfile_t *a, char b, int c, int d, int e) {
	return -ENODEV;
}


void chrdev_pipe_func(int (*fn)(chrfile_t *, chrfile_t *, int)) {
	chrdev_callbacks.pipe = fn;
}


void chrdev_dev_open_func(int (*fn)(chrfile_t *h, char, int, int, int)) {
	chrdev_callbacks.dev_open = fn;
}


void chrdev_close_func(int (*fn)(chrfile_t *)) {
	/* make it available to chrdev_rollback() */
	chrdev_callbacks.close = fn;
	io_close_func(fn);
}
