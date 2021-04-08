
#ifdef __sneks__

/* tests on the sneks file descriptor transfer interface.
 *
 * this module might eventually have more tests on descriptor ownership, but
 * for now it's just the transfers.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/api/io-defs.h>
#include <sneks/crtprivate.h>
#include <sneks/test.h>


/* FIXME: move this elsewhere, it's very useful */
#define A_SHORT_NAP L4_TimePeriod(10 * 1000)
#define TWO_SHORT_NAPS L4_TimePeriod(20 * 1000)


/* test of sending a handle to a different process.
 *
 * variables:
 *   - [use_dup_to] duplicate handle with dup_to, otherwise dup
 *   - [use_touch] acknowledge received handle with IO/touch
 *   - [use_pipe] make handle w/ pipe(2), otherwise w/ open(2)
 */
START_LOOP_TEST(transfer_ownership, iter, 0, 7)
{
	const bool use_dup_to = !!(iter & 1), use_touch = !!(iter & 2),
		use_pipe = !!(iter & 4);
	diag("use_dup_to=%s, use_touch=%s, use_pipe=%s",
		btos(use_dup_to), btos(use_touch), btos(use_pipe));
	plan(8);

	int fds[2], n;
	if(use_pipe) {
		n = pipe(fds);
		if(!ok(n == 0, "pipe(2)")) diag("errno=%d", errno);
		skip(1, "use_pipe=%s", btos(use_pipe));
	} else {
		skip(1, "use_pipe=%s", btos(use_pipe));
		fds[0] = open(TESTDIR "user/test/io/reg/testfile", O_RDONLY);
		if(!ok(fds[0] >= 0, "open(2)")) diag("errno=%d", errno);
		fds[1] = -1;
	}

	todo_start("unimplemented");

	L4_ThreadId_t parent = L4_MyGlobalId();
	int sub = fork_subtest_start("second process") {
		plan(6);

		/* receive a handle. */
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Call_Timeouts(parent, A_SHORT_NAP, A_SHORT_NAP);
		L4_ThreadId_t server; L4_StoreMR(1, &server.raw);
		L4_Word_t handle; L4_StoreMR(2, &handle);
		L4_Word_t error = L4_ErrorCode();
		if(!ok1(L4_IpcSucceeded(tag) && tag.X.u == 2)) {
			diag("tag=%#lx, ec=%lu", tag.raw, error);
		}

		skip_start(!use_touch, 2, "u can't touch this") {
			n = __io_touch(server, handle);
			bool fail = !imply_ok1(use_dup_to, n == 0);
			fail = !imply_ok1(!use_dup_to, n == -EBADF) || fail;
			if(fail) diag("Sneks::IO/touch returned n=%d", n);
		} skip_end;

		/* try to close it to determine validity. */
		n = __io_close(server, handle);
		bool fail = !imply_ok1(!use_dup_to || !use_touch, n == -EBADF);
		fail = !imply_ok1(use_dup_to && use_touch, n == 0) || fail;
		if(fail) diag("Sneks::IO/close returned n=%d", n);

		/* also close fds[0] to test the same for inherited descriptors. */
		n = close(fds[0]);
		error = errno;
		if(!ok(n == 0, "close(2) on inherited handle")) {
			diag("errno=%lu", error);
		}
	} fork_subtest_end;

	struct fd_bits *bits = __fdbits(fds[0]);
	skip_start(bits == NULL, 4, "no fdbits for fds[0]=%d??", fds[0]) {
		L4_MsgTag_t tag;
		L4_ThreadId_t sender = L4_nilthread;
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_Wait_Timeout(TWO_SHORT_NAPS, &sender);
		L4_Word_t ec = L4_ErrorCode();
		skip_start(
			!ok(L4_IpcSucceeded(tag) && pidof_NP(sender) == sub, "call from sub"),
			3, "ec=%lu or weird sender=%lu:%lu",
			ec, L4_ThreadNo(sender), L4_Version(sender))
		{
			L4_ThreadId_t server = bits->server;
			L4_Word_t handle = bits->handle;
			int new_handle = 0;
			if(use_dup_to) {
				skip(1, "not calling dup");
				n = __io_dup_to(server, &new_handle, handle, sub);
				if(!ok(n == 0, "Sneks::IO/dup_to")) diag("n=%d", n);
			} else {
				n = __io_dup(server, &new_handle, handle);
				if(!ok(n == 0, "Sneks::IO/dup")) diag("n=%d", n);
				skip(1, "not calling dup_to");
			}
			handle = new_handle;
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
			L4_LoadMR(1, server.raw);
			L4_LoadMR(2, handle);
			tag = L4_Reply(sender);
			ec = L4_ErrorCode();
			if(!ok(L4_IpcSucceeded(tag), "reply to sub")) diag("ec=%lu", ec);
		} skip_end;
	} skip_end;

	fork_subtest_ok1(sub);

	n = close(fds[0]);
	int err = errno;
	if(!ok(n == 0, "close(2)")) diag("errno=%d", err);

	if(use_pipe) close(fds[1]);
}
END_TEST

DECLARE_TEST("io:descxfer", transfer_ownership);

#endif
