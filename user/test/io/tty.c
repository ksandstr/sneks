/* tests on isatty(), and later on the TTY layer in general. */
#include <unistd.h>
#include <errno.h>
#include <sneks/test.h>

START_TEST(isatty_basic)
{
	plan_tests(3);

#ifndef __sneks__
	/* it's dicky to get a tty for test programs' stdout on a real proper unix
	 * host, so let's punt this one until the harness provides a pty instead.
	 */
	skip(1, "have pty");
#else
	if(!ok1(isatty(STDOUT_FILENO) == 1)) diag("errno=%d", errno);
#endif
	int pfd[2], n = pipe(pfd);
	fail_if(n < 0, "pipe(2) failed, errno=%d", errno);
	ok1(isatty(pfd[0]) == 0 && errno == ENOTTY);
	ok1(isatty(pfd[1]) == 0 && errno == ENOTTY);

	close(pfd[0]);
	close(pfd[1]);
}
END_TEST

DECLARE_TEST("io:tty", isatty_basic);
