
/* callback wrangling per <sneks/io.h> */

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/compiler/compiler.h>

#include <l4/types.h>

#include <sneks/api/io-defs.h>
#include <sneks/systask.h>
#include <sneks/io.h>

#include "private.h"


static int enosys();
static void no_lifecycle(pid_t, enum lifecycle_tag, ...);
static L4_Word_t dispatch_missing(void *);


struct io_callbacks callbacks = {
	.read = &enosys, .write = &enosys, .close = &enosys, .ioctl = &enosys,
	.lifecycle = &no_lifecycle, .confirm = NULL,
	.dispatch = &dispatch_missing,
};


static COLD int enosys() { return -ENOSYS; }

static void no_lifecycle(pid_t a, enum lifecycle_tag b, ...) {
	/* void */
}

static COLD L4_Word_t dispatch_missing(void *priv UNUSED) {
	log_crit("dispatcher missing (hard fail)");
	abort();
}


void io_read_func(int (*fn)(iof_t *, uint8_t *, unsigned, off_t)) {
	callbacks.read = fn != NULL ? fn : &enosys;
}


void io_write_func(int (*fn)(iof_t *, const uint8_t *, unsigned, off_t)) {
	callbacks.write = fn != NULL ? fn : &enosys;
}


void io_close_func(int (*fn)(iof_t *)) {
	callbacks.close = fn != NULL ? fn : &enosys;
}


void io_ioctl_func(int (*fn)(iof_t *, long, va_list)) {
	callbacks.ioctl = fn != NULL ? fn : &enosys;
}


void io_lifecycle_func(void (*fn)(pid_t, enum lifecycle_tag, ...)) {
	callbacks.lifecycle = fn != NULL ? fn : &no_lifecycle;
}


void io_confirm_func(void (*fn)(iof_t *, unsigned, off_t, bool)) {
	/* allows NULL. */
	callbacks.confirm = fn;
}


void _io_dispatch_func(L4_Word_t (*fn)(void *), const void *priv) {
	callbacks.dispatch = fn != NULL ? fn : &dispatch_missing;
	callbacks.dispatch_priv = (void *)priv;
}
