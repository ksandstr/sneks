
/* tests on the stdin/stdout/stderr channels, i.e. FDs 0 thru 2. */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sneks/test.h>


START_TEST(dup_and_close_stdin)
{
	plan(2);
#ifdef __sneks__
	todo_start("borked");
#endif

	int newfd = dup(0);
	if(!ok1(newfd >= 0)) diag("errno=%d", errno);

	skip_start(newfd < 0, 1, "no fd") {
		int n = close(newfd);
		if(!ok(n == 0, "close")) diag("errno=%d", errno);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stdfile", dup_and_close_stdin);


START_TEST(fcntl_dupfd)
{
	plan_tests(4);
	todo_start("nix");

	int n = fcntl(1, F_DUPFD, 10);
	if(!ok(n >= 10, "F_DUPFD(10)")) {
		diag("n=%d, errno=%d", n, errno);
	}
	if(n >= 0) close(n);

	n = fcntl(1, F_DUPFD, 0);
	if(!ok(n > 2, "F_DUPFD(0)")) {
		diag("n=%d, errno=%d", n, errno);
	}
	if(n >= 0) close(n);

	n = fcntl(1, F_DUPFD, 679);
	int n2 = fcntl(1, F_DUPFD, 678);
	if(!ok(n >= 678 && n2 >= 678, "F_DUPFD(high), twice")) {
		diag("n=%d, n2=%d, errno=%d", n, n2, errno);
	}
	imply_ok1(n >= 0 && n2 >= 0, n != n2);
	if(n >= 0) close(n);
	if(n2 >= 0) close(n2);
}
END_TEST

DECLARE_TEST("io:stdfile", fcntl_dupfd);
