
/* tests on the stdin/stdout/stderr channels, i.e. FDs 0 thru 2. */

#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>

#include <sneks/test.h>


START_TEST(dup_and_close_stdin)
{
	plan(2);

	int newfd = dup(0);
	if(!ok1(newfd >= 0)) diag("errno=%d", errno);
	else diag("newfd=%d", newfd);

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


/* duplicate a single file descriptor a hojillion times. then close it.
 * this is for provoking an issue particular to sneks, where server-side file
 * handles aren't duplicated but rather userspace must reference count its
 * descriptors.
 */
START_LOOP_TEST(many_dups, iter, 0, 1)
{
	const int n_dups = (iter & 1) ? 567 : 11;
	diag("n_dups=%d", n_dups);
	plan_tests(6);
#ifdef __sneks__
	todo_start("lol, n00b");
#endif

	int n = sigaction(SIGPIPE,
		&(struct sigaction){ .sa_handler = SIG_IGN }, NULL);
	fail_unless(n == 0, "sigaction: errno=%d", errno);

	int fds[2];
	n = pipe(fds);
	ok(n == 0, "pipe");

	bool dups_ok = true;
	int *dups = malloc(n_dups * sizeof *dups);
	for(int i=0; i < n_dups; i++) dups[i] = -1;
	for(int i=0; i < n_dups; i++) {
		dups[i] = dup(fds[0]);
		if(dups[i] < 0) {
			diag("dups[%d] failed, errno=%d", i, errno);
			dups_ok = false;
			break;
		}
	}
	ok1(dups_ok);

	bool close_ok = true;
	for(int i=0; i < n_dups; i++) {
		if(dups[i] <= 0) continue;
		n = close(dups[i]);
		if(n < 0) {
			diag("close(dups[%d]=%d) failed, errno=%d", i, dups[i], errno);
			close_ok = false;
			break;
		}
	}
	ok(close_ok, "dups closed");

	/* read side shouldn't yet have closed. */
	n = write(fds[1], &(char){ 'x' }, 1);
	ok(n == 1, "pipe not yet broken");

	ok1(close(fds[0]) == 0);

	/* verify that the read side has now closed. */
	n = write(fds[1], &(char){ 'x' }, 1);
	ok(n < 0 && errno == EPIPE, "pipe broken");

	close(fds[1]);
}
END_TEST

DECLARE_TEST("io:stdfile", many_dups);
