
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <sneks/test.h>


/* tests that setuid() works once and exactly once. requires euid=0 or skips
 * all.
 */
START_TEST(setuid_basic)
{
	if(geteuid() != 0) {
		plan_skip_all("geteuid() != 0");
		return;
	}

	plan_tests(5);

	int n = setuid(1000);
	if(!ok(n == 0, "setuid to 1000")) {
		diag("errno=%d", errno);
	}
	ok1(getuid() == 1000);
	n = setuid(1001);
	if(!ok(n < 0 && errno == EPERM, "setuid to 1001")) {
		diag("n=%d, errno=%d", n, errno);
	}
	ok1(getuid() == 1000);
	n = setuid(0);
	if(!ok(n < 0 && errno == EPERM, "setuid to 0")) {
		diag("n=%d, errno=%d", n, errno);
	}
}
END_TEST

DECLARE_TEST("process:setuid", setuid_basic);
