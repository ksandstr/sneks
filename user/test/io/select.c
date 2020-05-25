
/* tests about select(2) and poll(2). */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>

#if !defined(__sneks__) && !defined(EPOLLEXCLUSIVE)
/* not supported in every version of Linux; for the purposes of these tests,
 * removing the bit will be just as good.
 */
#define EPOLLEXCLUSIVE 0
#endif


/* listen for input through a single pipe from a single child process, using
 * select(2).
 *
 * variables:
 *   - [act_send] false for active wait, true for active send
 *
 * NB: this test is copied, pasted, and modified using epoll_from_pipe. any
 * bug in one may indicate the same bug in the other.
 */
START_LOOP_TEST(select_from_pipe, iter, 0, 1)
{
	const bool act_send = !!(iter & 1);
	diag("act_send=%s", btos(act_send));
	plan_tests(5);
#ifdef __sneks__
	todo_start("bar");
#endif

	int fds[2];
	int n = pipe(fds);
	fail_unless(n == 0, "pipe(2) failed, errno=%d", errno);

	int child = fork_subtest_start("data sender") {
		close(fds[0]);
		plan(1);
		if(act_send) usleep(10 * 1000);
		n = write(fds[1], &(char){ 'x' }, 1);
		ok(n == 1, "write");
		close(fds[1]);
	} fork_subtest_end;
	close(fds[1]);

	int flags = fcntl(fds[0], F_GETFL);
	n = fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	ok(n == 0, "F_SETFL (nonblock)");

	if(!act_send) usleep(10 * 1000);
	fd_set rd;
	do {
		FD_ZERO(&rd); FD_SET(fds[0], &rd);
		struct timeval timeout = { .tv_usec = 50 * 1000 };
		n = select(fds[0] + 1, &rd, NULL, NULL, &timeout);
	} while(n < 0 && errno == EINTR);
	if(!ok(n == 1, "select")) diag("n=%d, errno=%d", n, errno);
	ok1(FD_ISSET(fds[0], &rd));

	char buf;
	n = read(fds[0], &buf, 1);
	if(!ok(n == 1 && buf == 'x', "read 'x'")) {
		diag("n=%d, errno=%d, buf=`%c'", n, errno, buf);
	}

	fork_subtest_ok1(child);
	close(fds[0]);
}
END_TEST

DECLARE_TEST("io:nonblock", select_from_pipe);


/* write into multiple reader processes through nonblocking pipes,
 * select(2)ing between them.
 *
 * NB: same copypasta warning as the test prior.
 */
START_LOOP_TEST(select_write_many, iter, 0, 1)
{
	const int n_readers = (~iter & 1) ? 2 : 7;
	diag("n_readers=%d", n_readers);
	plan_tests(2);
#ifdef __sneks__
	todo_start("no no, no no there's no limit");
#endif

	struct sigaction act = { .sa_handler = SIG_IGN };
	int n = sigaction(SIGPIPE, &act, NULL);
	if(n < 0) BAIL_OUT("sigaction failed, errno=%d", errno);

	int readers[n_readers];
	struct { int fds[2]; } pipes[n_readers];
	for(int i=0; i < n_readers; i++) {
		int n = pipe(pipes[i].fds);
		if(n < 0) BAIL_OUT("pipe(2) failed, errno=%d", errno);

		readers[i] = fork_subtest_start("reader %d", i) {
			plan(2);
			for(int j=0; j <= i; j++) close(pipes[j].fds[1]);
			char *buf = malloc(8192);
			int total = 0, n;
			do {
				n = read(pipes[i].fds[0], buf, 8192);
				if(n > 0) total += n;
			} while(n > 0 || (n < 0 && errno == EINTR));
			if(!ok(n == 0, "got eof")) diag("n=%d, errno=%d", n, errno);
			if(!ok(total >= 128 * 1024, "got 128 KiB")) diag("total=%d", total);
			close(pipes[i].fds[0]);
			free(buf);
		} fork_subtest_end;

		close(pipes[i].fds[0]);
		int flags = fcntl(pipes[i].fds[1], F_GETFL);
		n = fcntl(pipes[i].fds[1], F_SETFL, flags | O_NONBLOCK);
		fail_if(n != 0, "fcntl[SETFL]: errno=%d", errno);
	}

	int not_done = n_readers;
	size_t remain[n_readers];
	int maxfd = -1;
	for(int i=0; i < n_readers; i++) {
		remain[i] = 128 * 1024;
		maxfd = max(maxfd, pipes[i].fds[1]);
	}

	/* main writer loop */
	char *buf = malloc(8192);
	memset(buf, 0xfe, 8192);
	while(not_done > 0) {
		struct timeval timeout = { .tv_usec = 50 * 1000 };
		fd_set wr;
		do {
			FD_ZERO(&wr);
			for(int i=0; i < n_readers; i++) {
				if(remain[i] <= 0) continue;
				FD_SET(pipes[i].fds[1], &wr);
			}
			n = select(maxfd + 1, NULL, &wr, NULL, &timeout);
		} while(n < 0 && errno == EINTR);
		if(n == 0) {
			diag("select timed out");
			break;
		} else if(n < 0) {
			diag("select failed, errno=%d", errno);
			break;
		}
		for(int i=0; i < n_readers; i++) {
			if(remain[i] <= 0) continue;
			if(!FD_ISSET(pipes[i].fds[1], &wr)) continue;
			do {
				n = write(pipes[i].fds[1], buf, 8192);
				if(n > 0) {
					remain[i] -= n;
					if(remain[i] <= 0 && remain[i] + n > 0) not_done--;
				}
			} while(remain[i] > 0 && (n > 0 || (n < 0 && errno == EINTR)));
			if(n < 0 && errno != EAGAIN) {
				diag("write to i=%d failed, errno=%d", i, errno);
				remain[i] = -1;
			}
		}
	}
	free(buf);
	ok(not_done == 0, "all sent");

	int fail = 0;
	for(int i=0; i < n_readers; i++) {
		close(pipes[i].fds[1]);
		int st = fork_subtest_join(readers[i]);
		fail |= !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	ok(!fail, "no failed readers");
}
END_TEST

DECLARE_TEST("io:nonblock", select_write_many);


/* listen for input through a single pipe from a single child process, using
 * poll(2).
 *
 * variables:
 *   - [act_send] false for active wait, true for active send
 *
 * NB: copypasta warning as in select_from_pipe!
 */
START_LOOP_TEST(poll_from_pipe, iter, 0, 1)
{
	const bool act_send = !!(iter & 1);
	diag("act_send=%s", btos(act_send));
	plan_tests(5);
#ifdef __sneks__
	todo_start("not implemented");
#endif

	int fds[2];
	int n = pipe(fds);
	fail_unless(n == 0, "pipe(2) failed, errno=%d", errno);

	int child = fork_subtest_start("data sender") {
		close(fds[0]);
		plan(1);
		if(act_send) usleep(10 * 1000);
		n = write(fds[1], &(char){ 'x' }, 1);
		ok(n == 1, "write");
		close(fds[1]);
	} fork_subtest_end;
	close(fds[1]);

	int flags = fcntl(fds[0], F_GETFL);
	n = fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	ok(n == 0, "F_SETFL (nonblock)");

	if(!act_send) usleep(10 * 1000);
	struct pollfd po = { .fd = fds[0], .events = POLLIN };
	do {
		n = poll(&po, 1, 50);
	} while(n < 0 && errno == EINTR);
	if(!ok(n == 1, "poll")) diag("n=%d, errno=%d", n, errno);
	ok1(po.revents & POLLIN);

	char buf;
	n = read(fds[0], &buf, 1);
	if(!ok(n == 1 && buf == 'x', "read 'x'")) {
		diag("n=%d, errno=%d, buf=`%c'", n, errno, buf);
	}

	fork_subtest_ok1(child);
	close(fds[0]);
}
END_TEST

DECLARE_TEST("io:nonblock", poll_from_pipe);


/* write into multiple reader processes through nonblocking pipes, selecting
 * between them using poll(2).
 *
 * NB: copypasta warning!
 */
START_LOOP_TEST(poll_write_many, iter, 0, 1)
{
	const int n_readers = (~iter & 1) ? 2 : 7;
	diag("n_readers=%d", n_readers);
	plan_tests(2);
#ifdef __sneks__
	todo_start("not implemented");
#endif

	struct sigaction act = { .sa_handler = SIG_IGN };
	int n = sigaction(SIGPIPE, &act, NULL);
	if(n < 0) BAIL_OUT("sigaction failed, errno=%d", errno);

	int readers[n_readers];
	struct { int fds[2]; } pipes[n_readers];
	for(int i=0; i < n_readers; i++) {
		int n = pipe(pipes[i].fds);
		if(n < 0) BAIL_OUT("pipe(2) failed, errno=%d", errno);

		readers[i] = fork_subtest_start("reader %d", i) {
			plan(2);
			for(int j=0; j <= i; j++) close(pipes[j].fds[1]);
			char *buf = malloc(8192);
			int total = 0, n;
			do {
				n = read(pipes[i].fds[0], buf, 8192);
				if(n > 0) total += n;
			} while(n > 0 || (n < 0 && errno == EINTR));
			if(!ok(n == 0, "got eof")) diag("n=%d, errno=%d", n, errno);
			if(!ok(total >= 128 * 1024, "got 128 KiB")) diag("total=%d", total);
			close(pipes[i].fds[0]);
			free(buf);
		} fork_subtest_end;

		close(pipes[i].fds[0]);
		int flags = fcntl(pipes[i].fds[1], F_GETFL);
		n = fcntl(pipes[i].fds[1], F_SETFL, flags | O_NONBLOCK);
		fail_if(n != 0, "fcntl[SETFL]: errno=%d", errno);
	}

	int not_done = n_readers;
	size_t remain[n_readers];
	struct pollfd po[n_readers];
	for(int i=0; i < n_readers; i++) {
		remain[i] = 128 * 1024;
		po[i].fd = pipes[i].fds[1];
		po[i].events = POLLOUT;
	}

	/* main writer loop */
	char *buf = malloc(8192);
	memset(buf, 0xfe, 8192);
	while(not_done > 0) {
		do {
			n = poll(po, ARRAY_SIZE(po), 50);
		} while(n < 0 && errno == EINTR);
		if(n == 0) {
			diag("poll timed out");
			break;
		} else if(n < 0) {
			diag("poll failed, errno=%d", errno);
			break;
		}
		for(int i=0; i < n_readers; i++) {
			if(remain[i] <= 0 || po[i].fd < 0) continue;
			fail_unless(po[i].fd == pipes[i].fds[1]);
			if(~po[i].revents & POLLOUT) continue;
			do {
				n = write(pipes[i].fds[1], buf, 8192);
				if(n > 0) {
					remain[i] -= n;
					if(remain[i] <= 0 && remain[i] + n > 0) {
						not_done--;
						po[i].fd = -po[i].fd;
					}
				}
			} while(remain[i] > 0 && (n > 0 || (n < 0 && errno == EINTR)));
			if(n < 0 && errno != EAGAIN) {
				diag("write to i=%d failed, errno=%d", i, errno);
				remain[i] = -1;
				po[i].fd = -po[i].fd;
			}
		}
	}
	free(buf);
	ok(not_done == 0, "all sent");

	int fail = 0;
	for(int i=0; i < n_readers; i++) {
		close(pipes[i].fds[1]);
		int st = fork_subtest_join(readers[i]);
		fail |= !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	ok(!fail, "no failed readers");
}
END_TEST

DECLARE_TEST("io:nonblock", poll_write_many);
