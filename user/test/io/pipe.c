
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/minmax/minmax.h>

#include <sneks/test.h>


/* very, very basic pipe(2) tests. just the interface really. */
START_LOOP_TEST(basic, iter, 0, 1)
{
	const bool use_pipe2 = !!(iter & 1);
	diag("use_pipe2=%s", btos(use_pipe2));
	plan_tests(7);

	int fds[2], n;
	if(!use_pipe2) n = pipe(fds); else n = pipe2(fds, 0);
	skip_start(!ok(n == 0, "pipe(2)"), 6, "no pipe, errno=%d", errno) {
		ok1(fds[0] > 0);
		ok1(fds[1] > 0);

		char c = 'x';	/* marks the spot */
		n = write(fds[1], &c, 1);
		if(!ok(n == 1, "write(2)")) diag("n=%d, errno=%d", n, errno);
		c = 0;
		n = read(fds[0], &c, 1);
		if(!ok(n == 1 && c == 'x', "read(2) w/ value")) {
			diag("n=%d, errno=%d, c=`%c'", n, errno, c);
		}

		for(int i=0; i < 2; i++) {
			n = close(fds[i]);
			if(!ok(n == 0, "close(fds[%d])", i)) diag("errno=%d", errno);
		}
	} skip_end;
}
END_TEST

DECLARE_TEST("io:pipe", basic);


/* EOF/broken pipe behaviour. */
START_LOOP_TEST(eof, iter, 0, 7)
{
	const bool writer_closes = !!(iter & 1),
		transmit_byte = !!(iter & 2), active_receive = !!(iter & 4);
	diag("writer_closes=%s, transmit_byte=%s, active_receive=%s",
		btos(writer_closes), btos(transmit_byte), btos(active_receive));
	plan_tests(5);

	struct sigaction act_pipe = { .sa_handler = SIG_IGN };
	int n = sigaction(SIGPIPE, &act_pipe, NULL);
	fail_if(n != 0, "errno=%d", errno);

	int fds[2];
	n = pipe(fds);
	if(!ok(n == 0, "pipe(2)")) diag("errno=%d", errno);

	pid_t child = fork_subtest_start("writer subtest") {
		plan(1);
		close(fds[0]);
		if(transmit_byte) {
			if(!active_receive) usleep(2 * 1000);
			char c = ':';	/* xD xD */
			n = write(fds[1], &c, 1);
			if(n != 1) diag("writer: transmit n=%d, errno=%d", n, errno);
		}
		skip_start(writer_closes, 1, "writer closes") {
			/* observe EPIPE as there should be no more readers left. */
			usleep(10 * 1000);
			char c = 0x42;
			n = write(fds[1], &c, 1);
			if(!ok1(n == -1 && errno == EPIPE)) {
				diag("writer: n=%d, errno=%d", n, errno);
			}
		} skip_end;
		close(fds[1]);
	} fork_subtest_end;

	close(fds[1]);
	skip_start(!transmit_byte, 2, "not transmitting") {
		if(active_receive) usleep(2 * 1000);
		char c = 0;
		n = read(fds[0], &c, 1);
		ok(n == 1, "byte was read");
		ok1(c == ':');
	} skip_end;

	skip_start(!writer_closes, 1, "reader closes") {
		/* observe EOF. */
		usleep(10 * 1000);
		char c = 0;
		n = read(fds[0], &c, 1);
		if(!ok(n == 0, "got EOF")) {
			diag("reader: n=%d, errno=%d", n, errno);
		}
	} skip_end;
	close(fds[0]);
	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("io:pipe", eof);


/* a single pipe with multiple readers. parent process acts as writer.
 *
 * shows that all readers receive a single byte that was sent by the parent,
 * demonstrating that reader sleeping and wakeups work even when one thread
 * goes away by segfault.
 */
START_LOOP_TEST(many_readers, iter, 0, 3)
{
	const bool first_reader_dies = !!(iter & 1);
	const int reader_count = (iter & 2) ? 11 : 3;
	diag("first_reader_dies=%s, reader_count=%d", btos(first_reader_dies),
		reader_count);
	plan_tests(2);

	struct sigaction act_pipe = { .sa_handler = SIG_IGN };
	int n = sigaction(SIGPIPE, &act_pipe, NULL);
	fail_if(n != 0, "errno=%d", errno);

	int fds[2];
	n = pipe2(fds, 0);
	fail_if(n != 0, "errno=%d", errno);
	int children[reader_count];
	for(int i=0; i < reader_count; i++) {
		int child = fork();
		if(child == 0) {
			close(fds[1]);
			char c;
			n = read(fds[0], &c, 1);
			bool good = n == 1 && c >= 0x42 && c < 0x42 + reader_count;
			if(i == 0 && first_reader_dies) {
				diag("child=%d (i=%d) gonna go down!", (int)getpid(), i);
				strscpy((void *)0xdeadbeef, "whatevs, man", 123);
				diag("shouldn't get here!!!");
				exit(2);
			}
			exit(!good);
		}
		children[i] = child;
	}
	close(fds[0]);

	for(int i=0; i < reader_count; i++) {
		char c = 0x42 + i;
		n = write(fds[1], &c, 1);
		fail_unless(n == 1, "n=%d, errno=%d", n, errno);
	}
	usleep(2 * 1000);
	close(fds[1]);

	int status[reader_count];
	for(int i=0; i < reader_count; i++) {
		int st, dead = waitpid(-1, &st, 0);
		fail_if(dead < 0, "errno=%d", errno);
		bool found = false;
		for(int j=0; j < reader_count; j++) {
			if(children[j] == dead) {
				children[j] = -1;
				status[j] = st;
				found = true;
				break;
			}
		}
		fail_unless(found);
	}

	bool rc_ok = true;
	imply_ok1(first_reader_dies,
		WIFSIGNALED(status[0]) && WTERMSIG(status[0]) == SIGSEGV);
	for(int i = first_reader_dies ? 1 : 0; i < reader_count; i++) {
		if(!WIFEXITED(status[i]) || WEXITSTATUS(status[i]) != 0) {
			diag("wrong status from i=%d", i);
			rc_ok = false;
		}
	}
	ok1(rc_ok);
}
END_TEST

DECLARE_TEST("io:pipe", many_readers);


/* send a megabyte of guff to a child process. */
START_LOOP_TEST(blocking_write, iter, 0, 1)
{
	const bool act_recv = !!(iter & 1);
	diag("act_recv=%s", btos(act_recv));
	plan_tests(2);

	struct sigaction ign = { .sa_handler = SIG_IGN };
	int n = sigaction(SIGPIPE, &ign, NULL);
	fail_if(n != 0, "sigaction failed, errno=%d", errno);

	int fds[2];
	n = pipe(fds);
	fail_if(n != 0, "pipe(2) failed, errno=%d", errno);

	int child = fork_subtest_start("receiver") {
		plan(2);
		close(fds[1]);
		const size_t bufsz = 1024 * 128;
		char *buf = malloc(bufsz);
		if(buf == NULL) BAIL_OUT("malloc");
		size_t total = 0, blocks = 0;
		if(act_recv) usleep(10 * 1000);
		do {
			n = read(fds[0], buf, bufsz);
			if(n > 0) {
				total += n;
				blocks++;
			}
		} while(n > 0 || (n < 0 && errno == EINTR));
		if(!ok(n == 0, "got eof")) diag("errno=%d", errno);
		ok(total == 1024 * 1024, "got a meg of data");
		diag("blocks=%d", (int)blocks);
		free(buf);
		close(fds[0]);
	} fork_subtest_end;
	close(fds[0]);

	int remain = 1024 * 1024;
	char *buf = malloc(16 * 1024);
	if(buf == NULL) BAIL_OUT("malloc");
	memset(buf, 0xda, 16 * 1024);
	if(!act_recv) usleep(10 * 1000);
	do {
		n = write(fds[1], buf, min_t(int, remain, 16 * 1024));
		if(n > 0) remain -= n;
	} while(remain > 0 && (n > 0 || (n < 0 && errno == EINTR)));
	if(!ok(remain == 0, "wrote all bytes")) diag("remain=%d", remain);
	free(buf);

	close(fds[1]);
	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("io:pipe", blocking_write);
