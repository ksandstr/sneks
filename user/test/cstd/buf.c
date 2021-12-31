/* tests on the setbuf(3) family. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/array_size/array_size.h>
#include <ccan/pipecmd/pipecmd.h>
#include <ccan/str/str.h>
#include <sneks/test.h>

static void setnb(int fd) {
	int n = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fail_if(n < 0, "fcntl(F_SETFL, ... | O_NONBLOCK) failed w/ errno=%d", errno);
}

static int readall(int fd, char *buf, size_t size) {
	int n, got = 0;
	do {
		n = read(fd, buf + got, size - got);
		if(n < 0) {
			if(errno == EAGAIN) break;
			if(errno == EINTR) continue;
			return -1;
		}
		got += n;
	} while(n > 0 && got < size);
	return got;
}

/* provide a pipe(2)'d outfd to a child process, instruct it to alter stdout
 * buffering, and examine its output.
 *
 * variables:
 *   - type of buffering to set: none, line, block ("full"), default.
 *   - whether to use setvbuf() or an alternate function where available.
 */
START_LOOP_TEST(pipe_output_buffering, iter, 0, 7)
{
	int buftype = iter & 3;
	const bool use_alt = !!(iter & 4);
	static const char *const buftypes[] = {
		[_IOFBF] = "block", [_IOLBF] = "line", [_IONBF] = "none",
		[3] = "default",
	};
	diag("buftype=%s, use_alt=%s", buftypes[buftype], btos(use_alt));
	plan_tests(7);
	char *buf = calloc(1, BUFSIZ);

#ifdef __sneks__
	todo_start("not implemented");
#endif

	int infd, outfd;
	assert(buftype < ARRAY_SIZE(buftypes) && buftypes[buftype] != NULL);
	pid_t child = pipecmd(&infd, &outfd, &pipecmd_preserve,
		TESTDIR "/user/test/tools/buffered_printer",
		buftypes[buftype], use_alt ? "alt" : "reg", NULL);
	fail_if(child <= 0, "pipecmd failed w/ errno=%d", errno);
	setnb(outfd);
	if(streq(buftypes[buftype], "default")) {
		/* default for pipes, not being terminals, is block buffering. */
		buftype = _IOFBF;
	}

	/* first part should arrive iff a "none" mode was specified, and wait
	 * otherwise.
	 */
	usleep(10000);
	int got = readall(outfd, buf, BUFSIZ);
	fail_if(got < 0, "readall: errno=%d", errno);
	//diag("got=%d, buf=`%s'", got, buf);
	iff_ok1(buftype == _IONBF, got == 6 && memcmp(buf, "first ", 6) == 0);

	/* second part should arrive on its own when unbuffered, after first if
	 * line buffered, and stay in a block buffer otherwise.
	 */
	write(infd, "0", 1); usleep(10000);
	memset(buf, '\0', BUFSIZ);
	got = readall(outfd, buf, BUFSIZ);
	fail_if(got < 0, "readall: errno=%d", errno);
	iff_ok1(buftype == _IONBF, memcmp(buf, "second\n", 7) == 0);
	iff_ok1(buftype == _IOLBF, got == 13 && memcmp(buf, "first second\n", 13) == 0);
	iff_ok1(buftype == _IOFBF, got == 0);

	/* last part should be empty when not buffered or line buffered, and all
	 * of the output when block buffered.
	 */
	write(infd, "1", 1); usleep(10000);
	memset(buf, '\0', BUFSIZ);
	got = readall(outfd, buf, BUFSIZ);
	fail_if(got < 0, "readall: errno=%d", errno);
	//diag("got=%d, buf=`%s'", got, buf);
	iff_ok1(buftype == _IOFBF, got == 13 && memcmp(buf, "first second\n", 13) == 0);
	iff_ok1(buftype != _IOFBF, got == 0);

	int st, n = waitpid(child, &st, 0);
	if(!ok(n == child, "waited")) {
		diag("waitpid(2) failed, errno=%d", errno);
	}

	free(buf);
}
END_TEST

DECLARE_TEST("cstd:buf", pipe_output_buffering);
