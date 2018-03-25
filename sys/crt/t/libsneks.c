
/* tests on stuff in libsneks.a that isn't involved enough to go anywhere
 * else. these have a different prefix than other stuff in sys/crt/t/ because
 * they're not part of the systask runtime but available to roottask and
 * sysmem also.
 */

#include <l4/types.h>
#include <l4/thread.h>

#include <sneks/process.h>
#include <sneks/test.h>


START_TEST(pidof_basic)
{
	plan_tests(5);

	ok1(pidof_NP(L4_Myself()) > 0);
	ok1(pidof_NP(L4_Myself()) != pidof_NP(L4_Pager()));

	ok(pidof_NP(L4_GlobalId(1, 1)) == 0,
		"interrupt=1:1 in forbidden range");
	ok(pidof_NP(L4_GlobalId(1023, 1)) == 0,
		"interrupt=1023:1 in forbidden range");
	ok(pidof_NP(L4_GlobalId(1024, 1)) == 0,
		"interrupt=1024:1 in forbidden range");
}
END_TEST


SYSTEST("lib:pidof", pidof_basic);
