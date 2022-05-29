#define CHRDEVIMPL_IMPL_SOURCE
#undef BUILD_SELFTEST

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/api/io-defs.h>
#include <sneks/process.h>
#include <sneks/rollback.h>
#include <sneks/systask.h>
#include <sneks/chrdev.h>

#include "muidl.h"
#include "chrdev-impl-defs.h"
#include "private.h"

struct open_rollback {
	chrfile_t *files[2];
};

static tss_t open_rollback_key;

static void init_chrdev(void) {
	if(tss_create(&open_rollback_key, &free) != thrd_success) abort();
}

static void chrdev_rollback(L4_Word_t x, struct open_rollback *ctx)
{
	for(int i=0; i < ARRAY_SIZE(ctx->files); i++) {
		if(ctx->files[i] != NULL) {
			int n = (*chrdev_callbacks.close)(ctx->files[i]);
			if(n != 0) log_err("close callback returned n=%d", n);
			iof_undo_new(ctx->files[i]);
		}
	}
}


static void add_rollback(chrfile_t *a, chrfile_t *b)
{
	struct open_rollback *ctx = tss_get(open_rollback_key);
	if(ctx == NULL) {
		ctx = malloc(sizeof *ctx);
		if(ctx == NULL) {
			log_crit("can't allocate <struct open_rollback>");
			abort();
		}
		tss_set(open_rollback_key, ctx);
	}
	*ctx = (struct open_rollback){ .files = { a, b } };
	set_rollback(&chrdev_rollback, 0, ctx);
}


/* Sneks::Pipe calls */

static int chrdev_pipe(int *rd_p, int *wr_p, int flags)
{
	int n;
	sync_confirm();

	int h_flags = 0, f_flags = 0;
	if(flags & O_CLOEXEC) h_flags |= IOD_CLOEXEC;
	if(flags & O_NONBLOCK) f_flags |= IOF_NONBLOCK;

	chrfile_t *readf = iof_new(f_flags), *writef = iof_new(f_flags);
	if(readf == NULL || writef == NULL) goto Enomem;

	n = (*chrdev_callbacks.pipe)(readf, writef, flags);
	if(n < 0) goto fail;

	pid_t caller = pidof_NP(muidl_get_sender());
	int rd = io_add_fd(caller, readf, h_flags),
		wr = io_add_fd(caller, writef, h_flags);
	if(rd < 0 || wr < 0) {
		n = min(rd, wr);
		goto fail;
	}

	add_rollback(readf, writef);
	*rd_p = rd;
	*wr_p = wr;
	return 0;

Enomem: n = -ENOMEM;
fail:
	iof_undo_new(readf); iof_undo_new(writef);
	assert(n < 0);
	return n;
}

static int chrdev_open(int *handle_p, unsigned object, L4_Word_t cookie, int flags)
{
	L4_MsgTag_t tag = muidl_get_tag();
	L4_ThreadId_t as = L4_ActualSender();
	sync_confirm();

	if(flags & ~(O_RDONLY | O_WRONLY | O_RDWR | O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
	if(pidof_NP(L4_IpcPropagated(tag) ? as : muidl_get_sender()) < SNEKS_MIN_SYSID) {
		/* TODO: validate cookie w/ key material from somewhere */
		return -EINVAL;
	}

	chrfile_t *file = iof_new(flags & O_NONBLOCK ? IOF_NONBLOCK : 0);
	if(file == NULL) return -ENOMEM;
	int n = (*chrdev_callbacks.dev_open)(file, "..c."[(object >> 30) & 0x3], major(object), minor(object), flags);
	if(n == 0) n = io_add_fd(pidof_NP(muidl_get_sender()), file, flags & O_CLOEXEC ? IOD_CLOEXEC : 0);
	if(n < 0) goto fail;

	add_rollback(file, NULL);
	*handle_p = n;
	return 0;
fail: iof_undo_new(file); return n;
}

static int enosys() { return -ENOSYS; }
static int espipe() { return -ESPIPE; }

int chrdev_run(size_t iof_size, int argc, char *argv[])
{
	static once_flag init = ONCE_FLAG_INIT;
	call_once(&init, &init_chrdev);
	struct chrdev_impl_vtable vtab = {
		/* Sneks::Pipe */
		.pipe = &chrdev_pipe,
		/* Sneks::File */
		.open = &chrdev_open, .seek = &espipe,
		/* Sneks::DeviceControl */
		.ioctl_void = &enosys, .ioctl_int = &enosys,
	};
	FILL_SNEKS_IO(&vtab);
	FILL_SNEKS_POLL(&vtab);
	io_dispatch_func(&_muidl_chrdev_impl_dispatch, &vtab);
	return io_run(iof_size, argc, argv);
}
