
/* tests on <sneks/rollback.h>, abort side. */

#define SNEKS_IO_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <threads.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/thread.h>
#include <sneks/test.h>
#include <sneks/rollback.h>
#include <sneks/api/io-defs.h>

#include "muidl.h"


struct iolimit_ctx {
	int limit;
};


static once_flag mod_init = ONCE_FLAG_INIT;
static tss_t iolimit_ctx_key;


static void mod_init_fn(void) {
	tss_create(&iolimit_ctx_key, free);
}


static void undo_write(L4_Word_t value, void *priv)
{
	struct iolimit_ctx *ctx = priv;
	ctx->limit += value;
}


static int iolimit_write(
	int fd, off_t offset, const uint8_t *buf, unsigned length)
{
	struct iolimit_ctx *ctx = tss_get(iolimit_ctx_key);
	if(ctx->limit == 0 && length > 0) return -EPIPE;
	assert(offset == -1);

	L4_Sleep(L4_TimePeriod(20 * 1000));

	int written = min_t(int, ctx->limit, length);
	ctx->limit -= written;
	set_rollback(&undo_write, written, ctx);
	return written;
}


static int iolimit_fn(void *param)
{
	call_once(&mod_init, &mod_init_fn);
	struct iolimit_ctx *ctx = malloc(sizeof *ctx);
	*ctx = (struct iolimit_ctx){
		.limit = (int)param,
	};
	tss_set(iolimit_ctx_key, ctx);

	static const struct sneks_io_vtable vtab = {
		.write = &iolimit_write,
		/* everything else: yolo! */
	};

	for(;;) {
		L4_Word_t status = _muidl_sneks_io_dispatch(&vtab);
		if(check_rollback(status)) continue;
		else if(status == MUIDL_UNKNOWN_LABEL) {
			L4_MsgTag_t tag = muidl_get_tag();
			if(L4_Label(tag) == 0xdead) break;
			printf("iolimit: unknown message label=%#lx, u=%lu, t=%lu\n",
				L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("iolimit: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}


static int cancel_receive_fn(void *param)
{
	L4_ThreadId_t target = { .raw = (L4_Word_t)param };
	L4_Sleep(L4_TimePeriod(8 * 1000));
	L4_ThreadState_t st = L4_AbortReceive_and_stop(target);
	L4_Start(target);
	return st.raw;
}


/* launch a helper implementation of Sneks::IO, which accepts writes up to a
 * given number of bytes, but sleeps for 20 milliseconds before replying. this
 * allows for a third thread to interrupt the main thread's __io_write() in
 * its receive phase, failing the operation.
 */
START_LOOP_TEST(broken_write, iter, 0, 1)
{
	const bool do_cancel = !!(iter & 1);
	diag("do_cancel=%s", btos(do_cancel));
	plan_tests(8);

	thrd_t iolimit;
	int n = thrd_create(&iolimit, &iolimit_fn, (void *)20);
	fail_if(n != thrd_success, "thrd_create: n=%d", n);

	uint16_t written = 0;
	const char sixteen[] = "01234567890abcdef";
	n = __io_write(thrd_to_tid(iolimit), &written, 0, -1,
		(const void *)sixteen, 16);
	diag("n=%d, written=%u", n, written);
	ok(n == 0, "IO::write");
	ok1(written == 16);

	thrd_t cancel;
	if(do_cancel) {
		n = thrd_create(&cancel, &cancel_receive_fn, (void *)L4_Myself().raw);
		fail_if(n != thrd_success, "thrd_create: n=%d", n);
	}
	written = 0;
	n = __io_write(thrd_to_tid(iolimit), &written, 0, -1,
		(const void *)sixteen, 16);
	diag("n=%d, written=%u", n, written);
	imply_ok1(!do_cancel, n == 0 && written == 4);
	imply_ok1(do_cancel, n == 7 || n == 11 || n == 13 || n == 15);

	skip_start(!do_cancel, 1, "no cancel helper") {
		/* clean up the cancel helper and validate its result */
		int res; n = thrd_join(cancel, &res);
		fail_unless(n == thrd_success, "n=%d", n);
		L4_ThreadState_t st = { .raw = res };
		if(!ok(L4_ThreadWasReceiving(st), "cancelled receive phase")) {
			diag("n=%d, st.raw=%#lx", n, st.raw);
		}
	} skip_end;

	written = 0;
	n = __io_write(thrd_to_tid(iolimit), &written, 0, -1,
		(const void *)sixteen, 16);
	diag("n=%d, written=%u", n, written);
	imply_ok1(!do_cancel, n == -EPIPE);
	imply_ok1(do_cancel, n == 0 && written == 4);

	/* clean up */
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xdead }.raw);
	L4_MsgTag_t tag = L4_Send(thrd_to_tid(iolimit));
	fail_if(L4_IpcFailed(tag), "ec=%lu", L4_ErrorCode());
	int res;
	n = thrd_join(iolimit, &res);
	ok(n == thrd_success, "helper joined");
}
END_TEST

SYSTEST("lib:rollback", broken_write);
