
/* tests on the wait(2), waitpid(2), and waitid(2) syscalls. */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <sneks/test.h>


START_TEST(empty_wait)
{
	plan_tests(2);

	int st, pid = wait(&st);
	ok1(pid == -1);
	ok1(errno == ECHILD);
}
END_TEST

DECLARE_TEST("process:wait", empty_wait);
