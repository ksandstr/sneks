
#define CHRDEVIMPL_IMPL_SOURCE
#undef BUILD_SELFTEST

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>

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

	if(flags != 0) return -EINVAL;

	/* TODO: parse pipe2(2) @flags into IOD_* and IOF_* */
	chrfile_t *readf = iof_new(0), *writef = iof_new(0);
	if(readf == NULL || writef == NULL) goto Enomem;

	n = (*chrdev_callbacks.pipe)(readf, writef, flags);
	if(n < 0) goto fail;

	pid_t caller = pidof_NP(muidl_get_sender());
	int rd = io_add_fd(caller, readf, 0), wr = io_add_fd(caller, writef, 0);
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


/* Sneks::DeviceNode calls */

static int chrdev_open(int *handle_p,
	uint32_t object, L4_Word_t cookie, int flags)
{
	sync_confirm();

	/* (we'll ignore @cookie because UAPI will have already checked it for us,
	 * and we don't have the key material anyway.)
	 */

	/* TODO: parse open(2) @flags into IOF_*, IOD_* */
	flags &= ~(O_RDONLY | O_WRONLY | O_RDWR);
	if(flags != 0) return -EINVAL;

	chrfile_t *file = iof_new(0);
	if(file == NULL) return -ENOMEM;

	static const char objtype[] = { [2] = 'c' };
	int n = (*chrdev_callbacks.dev_open)(file, objtype[(object >> 30) & 0x3],
		(object >> 15) & 0x7fff, object & 0x7fff, flags);
	if(n < 0) goto fail;

	n = io_add_fd(pidof_NP(muidl_get_sender()), file, 0);
	if(n < 0) goto fail;

	add_rollback(file, NULL);
	*handle_p = n;
	return 0;

fail:
	iof_undo_new(file);
	return n;
}


static int chrdev_ioctl_void(int *result_p, int handle, unsigned request)
{
	/* TODO */
	return -ENOSYS;
}


static int chrdev_ioctl_int(int *result_p,
	int handle, unsigned request, int *arg_p)
{
	/* TODO */
	return -ENOSYS;
}


int chrdev_run(size_t iof_size, int argc, char *argv[])
{
	int st = tss_create(&open_rollback_key, &free);
	if(st != thrd_success) {
		log_crit("tss_create failed, st=%d", st);
		return EXIT_FAILURE;
	}

	struct chrdev_impl_vtable vtab = {
		/* Sneks::Pipe */
		.pipe = &chrdev_pipe,

		/* Sneks::DeviceNode */
		.open = &chrdev_open,
		.ioctl_void = &chrdev_ioctl_void,
		.ioctl_int = &chrdev_ioctl_int,
	};
	FILL_SNEKS_IO(&vtab);
	FILL_SNEKS_POLL(&vtab);

	io_dispatch_func(&_muidl_chrdev_impl_dispatch, &vtab);
	return io_run(iof_size, argc, argv);
}
