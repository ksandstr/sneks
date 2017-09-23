
/* basic tests on C11 thrd_*() interfaces. */

#include <threads.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/test.h>


static int thread_fn(void *param_ptr)
{
	if(param_ptr == NULL) L4_Sleep(L4_TimePeriod(1000 * 2));
	diag("%s: executing! param_ptr=%p", __func__, param_ptr);
	return 666;
}


START_LOOP_TEST(create_join_exit, iter, 0, 1)
{
	const bool parent_sleep = (iter & 1) != 0;
	diag("parent_sleep=%s", btos(parent_sleep));

	diag("hello, world!");
	plan_tests(3);

	thrd_t t;
	int n = thrd_create(&t, &thread_fn, parent_sleep ? "foo" : NULL);
	if(parent_sleep) L4_Sleep(L4_TimePeriod(1000 * 2));
	ok(n == thrd_success, "thrd_create");

	int res = 0;
	n = thrd_join(t, &res);
	if(!ok(n == thrd_success, "thrd_join")) diag("n=%d", n);
	ok1(res == 666);
}
END_TEST


SYSTEST("crt:thrd", create_join_exit);
