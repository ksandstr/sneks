
/* tests on nonblocking I/O, i.e. select(2) and the like. */

#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <sneks/test.h>


static void silent_handler(int signo) {
	/* naught */
	diag("%s: runs", __func__);
}


/* set up a read() call of a pipe with no data in it. depending on O_NONBLOCK,
 * it should return either EAGAIN immediately or EINTR when interrupted by a
 * helper process.
 */
START_LOOP_TEST(nonblock_basic, iter, 0, 1)
{
	const bool nonblock = !!(iter & 1);
	diag("nonblock=%s", btos(nonblock));
	plan_tests(5);

	struct sigaction act = { .sa_handler = &silent_handler };
	int n = sigaction(SIGUSR1, &act, NULL);
	fail_unless(n == 0, "sigaction");

	int parent = getpid();
	int child = fork_subtest_start("signal parent after delay") {
		plan(1);
		usleep(10 * 1000);
		n = kill(parent, SIGUSR1);
		if(!ok(n == 0, "kill")) diag("n=%d, errno=%d", n, errno);
	} fork_subtest_end;

	int fds[2];
	n = pipe(fds);
	skip_start(!ok(n == 0, "pipe"), 3, "no pipe") {
		int flags = fcntl(fds[0], F_GETFL);
		if(flags >= 0) {
			n = fcntl(fds[0], F_SETFL,
				nonblock ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
		}
		if(!ok(flags >= 0 && n == 0, "fcntl get-setfl")) {
			diag("errno=%d", errno);
		}

		char buf;
		n = read(fds[0], &buf, 1);
		imply_ok1(nonblock, n == -1 && errno == EAGAIN);
		imply_ok1(!nonblock, n == -1 && errno == EINTR);
		if(n != -1 || (errno != EAGAIN && errno != EINTR)) {
			diag("n=%d, errno=%d", n, errno);
		}

		close(fds[0]); close(fds[1]);
	} skip_end;

	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("io:nonblock", nonblock_basic);
