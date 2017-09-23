
/* systest's meta tests. yeah, you heard me. */

#include <stdbool.h>
#include <sneks/test.h>


START_TEST(always_succeeds)
{
	plan_tests(1);
	ok1(true);
}
END_TEST


START_LOOP_TEST(always_succeeds_iter, iter, 0, 3)
{
	plan_tests(2);
	diag("iter=%d", iter);
	for(int i=0; i < 2; i++) pass("i=%d", i);
}
END_TEST


SYSTEST("systest:meta", always_succeeds);
SYSTEST("systest:meta", always_succeeds_iter);
