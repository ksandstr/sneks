
/* tests on the Sneks::Path/resolve cookie output parameter. */

#ifdef __sneks__

#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <ccan/str/str.h>

#include <l4/types.h>
#include <l4/thread.h>

#include <sneks/test.h>
#include <sneks/sysinfo.h>
#include <sneks/crtprivate.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/file-defs.h>


/* try pie, try. */
static int try_cookie(unsigned object, L4_ThreadId_t server, L4_Word_t cookie)
{
	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_ThreadId_t actual = L4_nilthread;
	int handle, n = __file_open(server, &handle, object, cookie, 0);
	actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;
	if(n == 0) __io_close(server, handle);
	return n;
}


/* tests on a valid cookie wrt Sneks::DeviceNode.
 *
 * variables:
 *   - allow the cookie to expire?
 *   - use it on a different object?
 */
START_LOOP_TEST(valid_dev_cookie, iter, 0, 3)
{
	const char *test_path = "dev/null";
	const bool allow_expire = !!(iter & 1), diff_object = !!(iter & 2);
	diag("test_path=`%s', allow_expire=%s, diff_object=%s", test_path,
		btos(allow_expire), btos(diff_object));
	plan(5);

	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, test_path, 0);
	if(!ok(n == 0, "path resolved")) {
		diag("n=%d", n);
	}

	skip_start(!diff_object, 2, "using same object") {
		const char *diff_path = "dev/zero";
		assert(!streq(diff_path, test_path));
		unsigned old_obj = object;
		n = __path_resolve(__the_sysinfo->api.rootfs, &object,
			&server.raw, &ifmt, &(L4_Word_t){ 0 }, 0, diff_path, 0);
		if(!ok(n == 0, "resolve diff_path=`%s'", diff_path)) {
			diag("n=%d", n);
		}
		ok1(old_obj != object);
	} skip_end;
	if(allow_expire) {
		const int delay = 120;
		diag("sleeping for %d ms...", delay);
		usleep(delay * 1000);
	}

	n = try_cookie(object, server, cookie);
	diag("try_cookie returned n=%d", n);
	iff_ok1(n == 0, !allow_expire && !diff_object);
	imply_ok1(diff_object || allow_expire, n == -EINVAL);
}
END_TEST

DECLARE_TEST("io:cookie", valid_dev_cookie);


/* test that an invalid path cookie, derived from a valid one, isn't accepted
 * for Sneks::DeviceNode. no variables.
 */
START_TEST(invalid_dev_cookie)
{
	const char *test_path = "dev/null";
	diag("test_path=`%s'", test_path);
	plan(3);

	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, test_path, 0);
	if(!ok(n == 0, "path resolved")) {
		diag("n=%d", n);
	}

	cookie ^= 0xdeadbeef;	/* well and truly fuxx0r'd */

	n = try_cookie(object, server, cookie);
	diag("try_cookie returned n=%d", n);
	ok(n < 0, "cookie not accepted");
	ok1(n == -EINVAL);
}
END_TEST

DECLARE_TEST("io:cookie", invalid_dev_cookie);


/* a cookie should stop working after the first go.
 *
 * TODO: this is not valid for device cookies since there's currently no way
 * to invalidate them after use aside from the cookie period. once filesystems
 * start handing out cookies for ordinary objects, change this to use that.
 * the device-specific behaviour is not worth testing for.
 */
#if 0
START_TEST(valid_cookie_twice)
{
	const char *test_path = "dev/null";
	diag("test_path=`%s'", test_path);
	plan(4);

	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, test_path, 0);
	if(!ok(n == 0, "path resolved")) {
		diag("n=%d", n);
	}

	todo_start("impl missing");

	n = try_cookie(object, server, cookie);
	if(!ok(n == 0, "accepted on first try")) {
		diag("n=%d", n);
	}

	n = try_cookie(object, server, cookie);
	ok(n < 0, "rejected on second try");
	ok1(n == -EINVAL);
}
END_TEST

DECLARE_TEST("io:cookie", valid_cookie_twice);
#endif

#endif /* __sneks__ */
