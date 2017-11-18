
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


/* tests basic thread creation and joining with a single thread. forces either
 * passive or active join (main thread's perspective) according to @iter.
 */
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


static int exit_from_param(void *param) {
	int val = *(int *)param;
	free(param);
	return val;
}


/* tests thread creation and joining using multiple threads. @iter changes the
 * number of threads (2, 7) and the join order (low to high or reverse).
 */
START_LOOP_TEST(create_join_many, iter, 0, 3)
{
	const bool many = !!(iter & 1), reverse = !!(iter & 2);
	diag("many=%s, reverse=%s", btos(many), btos(reverse));
	const int nt = many ? 7 : 2;
	diag("nt=%d", nt);
	plan_tests(1);

	diag("spawning...");
	thrd_t t[nt];
	int vals[nt];
	for(int i=0; i < nt; i++) {
		int *p = malloc(sizeof *p);
		vals[i] = *p = i + 100;
		int n = thrd_create(&t[i], &exit_from_param, p);
		fail_unless(n == thrd_success);
	}

	diag("joining...");
	bool vals_ok = true;
	for(int i=0; i < nt; i++) {
		int off = reverse ? nt - 1 - i : i, expect = vals[off];
		int res = 0, n = thrd_join(t[off], &res);
		fail_unless(n == thrd_success);
		if(res != expect) {
			diag("res=%d, expect=%d", res, expect);
			vals_ok = false;
		}
	}

	ok1(vals_ok);
}
END_TEST


SYSTEST("crt:thrd", create_join_exit);
SYSTEST("crt:thrd", create_join_many);
