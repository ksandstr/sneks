
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
	plan_tests(6);

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
	int n3 = fcntl(1, F_DUPFD, 678);
	if(!ok(n >= 678 && n2 >= 678 && n3 >= 678, "F_DUPFD(high), thrice")) {
		diag("n=%d, n2=%d, n3=%d, errno=%d", n, n2, n3, errno);
	}
	imply_ok1(n >= 0 && n2 >= 0, n != n2);
	imply_ok1(n >= 0 && n3 >= 0, n != n3);
	imply_ok1(n2 >= 0 && n3 >= 0, n2 != n3);
	if(n >= 0) close(n);
	if(n2 >= 0) close(n2);
	if(n3 >= 0) close(n3);
}
END_TEST

DECLARE_TEST("io:stdfile", fcntl_dupfd);


/* duplicate a single file descriptor a hojillion times. then close it.
 * this is for provoking an issue particular to sneks, where server-side file
 * handles aren't duplicated but rather userspace must reference count its
 * descriptors.
 */
START_LOOP_TEST(many_dups, iter, 0, 3)
{
	const int n_dups = (iter & 1) ? 567 : 11;
	const bool use_pipe = !!(iter & 2);
	diag("n_dups=%d, use_pipe=%s", n_dups, btos(use_pipe));
	plan_tests(8);

	todo_start("impl of Sneks::IO/dup missing from squashfs, libsneks-chrdev.a");

	int n = sigaction(SIGPIPE,
		&(struct sigaction){ .sa_handler = SIG_IGN }, NULL);
	fail_unless(n == 0, "sigaction: errno=%d", errno);

	int fds[2] = { -1, -1 };
	if(use_pipe) {
		n = pipe(fds);
		if(!ok(n == 0, "pipe(2)")) diag("errno=%d", errno);
		skip(1, "use_pipe=%s", btos(use_pipe));
	} else {
		skip(1, "use_pipe=%s", btos(use_pipe));
		fds[0] = open(TESTDIR "/user/test/io/reg/testfile", O_RDONLY);
		if(!ok(fds[0] >= 0, "open(2)")) diag("errno=%d", errno);
	}

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

	skip_start(!use_pipe, 1, "use_pipe=%s", btos(use_pipe)) {
		/* read side shouldn't yet have closed. */
		n = write(fds[1], &(char){ 'x' }, 1);
		ok(n == 1, "pipe not yet broken");
	} skip_end;

	ok1(close(fds[0]) == 0);

	skip_start(!use_pipe, 2, "use_pipe=%s", btos(use_pipe)) {
		/* verify that the read side has now closed. */
		n = write(fds[1], &(char){ 'x' }, 1);
		ok(n < 0 && errno == EPIPE, "pipe broken");

		ok1(close(fds[1]) == 0);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stdfile", many_dups);
