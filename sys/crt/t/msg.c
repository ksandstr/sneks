
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <l4/types.h>

#include <sneks/test.h>
#include <sneks/msg.h>


#define TEST_BIT (1 << 30)


static bool counting_handler(int bit, L4_Word_t *body, int length, void *priv)
{
	if(bit != ffsl(TEST_BIT) - 1) {
		fprintf(stderr, "%s: bit=%d, expected %d\n", __func__,
			bit, ffsl(TEST_BIT) - 1);
	} else {
		atomic_fetch_add((_Atomic int *)priv, 1);
	}
	return true;
}


/* the most basic test. variable chooses between filters or not. */
START_LOOP_TEST(listen_send_close, iter, 0, 1)
{
	const bool filters = !!(iter & 1);
	diag("filters=%s", btos(filters));
	plan_tests(8);

	static _Atomic int counter;

	counter = 0;
	int handle = sysmsg_listen(ffsl(TEST_BIT) - 1,
		&counting_handler, &counter);
	if(!ok(handle >= 0, "listen")) {
		diag("handle=%d", handle);
	}

	const L4_Word_t body = 0xfadedbee;

	skip_start(!filters, 1, "filters not used") {
		int n = sysmsg_add_filter(handle, &body, 1);
		ok(n == 0, "add filter");
	} skip_end;

	int n = sysmsg_broadcast(TEST_BIT, 0, &body, 1);
	if(!ok(n == 0, "broadcast %#lx", body)) diag("n=%d", n);
	L4_Word_t another = 0xbeef00de;
	n = sysmsg_broadcast(TEST_BIT, 0, &another, 1);
	if(!ok(n == 0, "broadcast %#lx", another)) diag("n=%d", n);

	/* this assumes that a sole entry in the filter will be exact. that's
	 * probably the case, but if it isn't, change the values of "body" and
	 * "another" around until it is.
	 */
	imply_ok1(!filters, counter == 2);
	imply_ok1(filters, counter == 1);
	diag("counter=%d", counter);

	if(filters) sysmsg_rm_filter(handle, &body, 1);
	n = sysmsg_close(handle);
	if(!ok(n == 0, "close")) {
		diag("n=%d", n);
	}

	int oldcounter = atomic_load(&counter);
	L4_Word_t body2 = 0xf00dfeed;
	n = sysmsg_broadcast(TEST_BIT, 0, &body2, 1);
	if(!ok(n == 0 && atomic_load(&counter) == oldcounter,
		"broadcast after close had no effect"))
	{
		diag("n=%d, counter=%d", n, counter);
	}
}
END_TEST

SYSTEST("crt:msg", listen_send_close);


static bool handler_which_calls_listen_fn(
	int bit, L4_Word_t *body, int length, void *priv)
{
	diag("calling inner listen");
	if(bit != ffsl(TEST_BIT) - 1) return true;
	*(int *)priv = sysmsg_listen(ffsl(TEST_BIT) - 2,
		&handler_which_calls_listen_fn, NULL);
	if(*(int *)priv < 0) {
		diag("inner listen failed, n=%d", *(int *)priv);
		return true;
	}

	diag("calling inner add filter");
	L4_Word_t label = 0xdecafbe7;
	int n = sysmsg_add_filter(*(int *)priv, &label, 1);
	if(n != 0) diag("inner add filter failed, n=%d", n);
	else {
		diag("calling inner rm filter");
		n = sysmsg_rm_filter(*(int *)priv, &label, 1);
		if(n != 0) diag("inner rm filter failed, n=%d", n);
	}

	diag("calling inner close");
	sysmsg_close(*(int *)priv);
	*(int *)priv = 0;

	return true;
}


/* test that sysmsg_listen() and sysmsg_add_filter() can be called from within
 * sysmsg handlers. this requires that the Sysmsg implementation respond to
 * IDL stuff from a different thread from the one that waits for handlers to
 * complete, and that the systask runtime is sufficiently thread-safe.
 */
START_TEST(listen_from_handler)
{
	plan_tests(3);

	int *state = malloc(sizeof *state);
	*state = 1;
	int handle = sysmsg_listen(ffsl(TEST_BIT) - 1,
		&handler_which_calls_listen_fn, state);
	skip_start(!ok1(handle >= 0), 2, "no handle") {
		L4_Word_t foo = 0x12345678;
		int n = sysmsg_broadcast(TEST_BIT, 0, &foo, 1);
		ok(n >= 0, "broadcast");
		sysmsg_close(handle);
		if(!ok1(*state == 0)) diag("*state=%d", *state);
	} skip_end;

	free(state);
}
END_TEST

SYSTEST("crt:msg", listen_from_handler);


static bool handler_which_calls_broadcast_fn(
	int bit, L4_Word_t *body, int body_len, void *priv)
{
	L4_Word_t foo = 0x87654321;
	*(int *)priv = sysmsg_broadcast(TEST_BIT, 0, &foo, 1);
	return true;
}


/* test that sysmsg_broadcast() from within a handler returns -EDEADLK, which
 * it should.
 */
START_TEST(broadcast_from_handler)
{
	plan_tests(3);

	int *status = malloc(sizeof *status);
	*status = 0;

	int handle = sysmsg_listen(ffsl(TEST_BIT) - 1,
		&handler_which_calls_broadcast_fn, status);
	skip_start(!ok1(handle >= 0), 1, "no handle") {
		L4_Word_t foo = 0xabcdef01;
		int n = sysmsg_broadcast(TEST_BIT, 0, &foo, 1);
		if(!ok(n == 0, "broadcast")) diag("n=%d", n);
		sysmsg_close(handle);
	} skip_end;

	ok1(*status == -EDEADLK);

	free(status);
}
END_TEST

SYSTEST("crt:msg", broadcast_from_handler);
