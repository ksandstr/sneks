
/* tests on nonblocking I/O, i.e. select(2), poll(2), epoll(7), etc. */

#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/array_size/array_size.h>
#include <ccan/minmax/minmax.h>

#include <sneks/test.h>

#if !defined(__sneks__) && !defined(EPOLLEXCLUSIVE)
/* not supported in every version of Linux; for the purposes of these tests,
 * removing the bit will be just as good.
 */
#define EPOLLEXCLUSIVE 0
#endif


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


/* most basic part of the interface: creation and deletion of epollfds. */
START_TEST(epoll_basic)
{
	plan_tests(4);
#ifdef __sneks__
	todo_start("foo");
#endif

	int epfd = epoll_create1(0);
	if(!ok(epfd >= 0, "epoll_create1")) diag("errno=%d", errno);

	struct epoll_event dummy;
	int n = epoll_ctl(epfd, EPOLL_CTL_DEL, 0, &dummy);
	if(!ok(n == -1 && errno == ENOENT, "epoll_ctl before close")) {
		diag("n=%d, errno=%d", n, errno);
	}

	n = close(epfd);
	if(!ok(n == 0, "close")) diag("errno=%d", errno);

	n = epoll_ctl(epfd, EPOLL_CTL_DEL, 0, &dummy);
	if(!ok(n == -1 && errno == EBADF, "epoll_ctl after close")) {
		diag("n=%d, errno=%d", n, errno);
	}
}
END_TEST

DECLARE_TEST("io:nonblock", epoll_basic);


/* listen for input through a single pipe from a single child process, using
 * edge-triggered signaling. ignores EOF (or HUP in epoll terms).
 *
 * variables:
 *   - [act_send] false for active wait, true for active send
 */
START_LOOP_TEST(epoll_from_pipe, iter, 0, 1)
{
	const bool act_send = !!(iter & 1);
	diag("act_send=%s", btos(act_send));
	plan_tests(10);
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
		usleep(15 * 1000);
		close(fds[1]);
	} fork_subtest_end;
	close(fds[1]);

	int flags = fcntl(fds[0], F_GETFL);
	n = fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	ok(n == 0, "F_SETFL (nonblock)");

	int epfd = epoll_create1(0);
	n = epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &(struct epoll_event){
		.events = EPOLLET | EPOLLEXCLUSIVE | EPOLLIN, .data.u32 = 666 });
	if(!ok(n == 0, "epoll_ctl (add)")) diag("errno=%d", errno);

	if(!act_send) usleep(10 * 1000);
	struct epoll_event evs[2];
	do {
		n = epoll_wait(epfd, evs, ARRAY_SIZE(evs), 50);
	} while(n < 0 && errno == EINTR);
	if(!ok(n == 1, "epoll_wait")) diag("n=%d, errno=%d", n, errno);
	ok1(evs[0].events & EPOLLIN);
	ok1(evs[0].data.u32 == 666);	/* ave satanas */

	char buf;
	n = read(fds[0], &buf, 1);
	if(!ok(n == 1 && buf == 'x', "read 'x'")) {
		diag("n=%d, errno=%d, buf=`%c'", n, errno, buf);
	}

	/* catch EPOLLHUP as well, since we're down here. */
	memset(evs, '\0', sizeof evs);
	fail_unless(evs[0].data.u32 != 666);
	do {
		n = epoll_wait(epfd, evs, ARRAY_SIZE(evs), 50);
	} while(n < 0 && errno == EINTR);
	if(!ok(n == 1, "epoll_wait")) diag("n=%d, errno=%d", n, errno);
	ok1(evs[0].events & EPOLLHUP);
	ok1(evs[0].data.u32 == 666);	/* vicarius filii dei */

	fork_subtest_ok1(child);

	close(epfd);
	close(fds[0]);
}
END_TEST

DECLARE_TEST("io:nonblock", epoll_from_pipe);


/* write into multiple reader processes through nonblocking pipes, selecting
 * between them using epoll.
 */
START_LOOP_TEST(epoll_write_many, iter, 0, 1)
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

	int epfd = epoll_create1(0);
	if(epfd < 0) diag("epoll_create1() failed, errno=%d", errno);
	size_t remain[n_readers];
	int not_done = n_readers;
	for(int i=0; i < n_readers; i++) {
		remain[i] = 128 * 1024;
		n = epoll_ctl(epfd, EPOLL_CTL_ADD, pipes[i].fds[1],
			&(struct epoll_event){
				.events = EPOLLOUT | EPOLLET | EPOLLEXCLUSIVE,
				.data.u32 = i,
			});
		if(n < 0) diag("epoll_ctl[add] failed, errno=%d", errno);
	}

	/* main writer loop */
	char *buf = malloc(8192);
	memset(buf, 0xfe, 8192);
	while(not_done > 0) {
		struct epoll_event evs[n_readers];
		do {
			n = epoll_wait(epfd, evs, n_readers, 50);
		} while(n < 0 && errno == EINTR);
		if(n == 0) {
			diag("epoll_wait timed out");
			break;
		} else if(n < 0) {
			diag("epoll_wait failed, errno=%d", errno);
			break;
		}
		for(int i=0, n_evs = n; i < n_evs; i++) {
			if(~evs[i].events & EPOLLOUT) {
				diag("confusing events=%#x when i=%d", evs[i].events, i);
				continue;
			}
			int r = evs[i].data.u32;
			fail_unless(r < n_readers);
			do {
				n = write(pipes[r].fds[1], buf, 8192);
				if(n > 0) {
					remain[r] -= n;
					if(remain[r] <= 0 && remain[r] + n > 0) not_done--;
				}
			} while(remain[r] > 0 && (n > 0 || (n < 0 && errno == EINTR)));
			if(n < 0 && errno != EAGAIN) {
				diag("write to r=%d failed, errno=%d", r, errno);
				epoll_ctl(epfd, EPOLL_CTL_DEL, pipes[r].fds[1],
					&(struct epoll_event){ });
			}
		}
	}
	free(buf);
	ok(not_done == 0, "all sent");

	close(epfd);
	int fail = 0;
	for(int i=0; i < n_readers; i++) {
		close(pipes[i].fds[1]);
		int st = fork_subtest_join(readers[i]);
		fail |= !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	ok(!fail, "no failed readers");
}
END_TEST

DECLARE_TEST("io:nonblock", epoll_write_many);
