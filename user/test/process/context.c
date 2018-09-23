
/* tests on setcontext, getcontext, etc. */

#include <stdbool.h>
#include <ucontext.h>
#include <sneks/test.h>


START_TEST(basic_get_set)
{
	plan_tests(2);

	volatile int count = 0;
	ucontext_t ctx;
	getcontext(&ctx);
	if(count++ == 0) {
		diag("calling setcontext at count=%d", count);
		setcontext(&ctx);
	}
	if(!ok(count == 2, "getcontext() returned twice")) {
		diag("count=%d", count);
	}
	pass("didn't crash afterward");
}
END_TEST

DECLARE_TEST("process:context", basic_get_set);
