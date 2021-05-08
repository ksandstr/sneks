
/* big test on FD_CLOEXEC. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>

#ifdef __sneks__
#include <l4/types.h>
#include <sneks/crtprivate.h>
#endif


struct fdgen {
	const char *name;
	int (*fn)(void *param);
	void *param;
	bool sets_cloexec;
};


static int gen_open(void *);
static int gen_open_c(void *);
static int gen_openat(void *);
static int gen_openat_c(void *);
static int gen_pipe(void *);
static int gen_pipe2(void *);
static int gen_dup(void *);
static int gen_dup2(void *);
static int gen_dup3(void *);


/* TODO: block device, fifo, socket, epoll_create1, fcntl(F_DUPFD),
 * fcntl(F_DUPFD_CLOEXEC)
 */
static const struct fdgen gens[] = {
	{ "open regular", &gen_open, TESTDIR "/user/test/io/dir/0", false },
	{ "open directory", &gen_open, TESTDIR "/user/test/io/dir/a", false },
	{ "open character device", &gen_open, "/dev/zero", false },
	{ "open regular (C)", &gen_open_c, TESTDIR "/user/test/io/dir/0", true },
	{ "open directory (C)", &gen_open_c, TESTDIR "/user/test/io/dir/a", true },
	{ "open character device (C)", &gen_open_c, "/dev/zero", true },
	{ "openat regular", &gen_openat, TESTDIR "/user/test/io/dir/0", false },
	{ "openat directory", &gen_openat, TESTDIR "/user/test/io/dir/a", false },
	{ "openat character device", &gen_openat, "/dev/zero", false },
	{ "openat regular (C)", &gen_openat_c, TESTDIR "/user/test/io/dir/0", true },
	{ "openat directory (C)", &gen_openat_c, TESTDIR "/user/test/io/dir/a", true },
	{ "openat character device (C)", &gen_openat_c, "/dev/zero", true },
	{ "pipe", &gen_pipe, NULL, false },
	{ "pipe2", &gen_pipe2, NULL, false },
	{ "pipe2 (C)", &gen_pipe2, "", true },
	/* TODO: variations on whether the descriptor being duplicated has
	 * FD_CLOEXEC or not; when O_CLOEXEC isn't given to dup3(), the resulting
	 * descriptor should have it cleared.
	 */
	{ "dup", &gen_dup, NULL, false },
	{ "dup2", &gen_dup2, NULL, false },
	{ "dup3", &gen_dup3, NULL, false },
	{ "dup3 (C)", &gen_dup3, "", true },
};


START_LOOP_TEST(cloexec, iter, 0, 2 * ARRAY_SIZE(gens) - 1)
{
	const bool invert = !!(iter & 1);
	const struct fdgen *gen = &gens[iter >> 1];
	diag("invert=%s, gen=`%s'", btos(invert), gen->name);
	plan_tests(12);

	/* force testfd to a higher range so that when sneks userspace crt1 calls
	 * fchdir() the duplicated descriptor doesn't land on our test
	 * descriptor's slot.
	 */
	int spam[32];
	bool prep_ok = true;
	for(int i=0; i < ARRAY_SIZE(spam); i++) {
		spam[i] = dup(STDOUT_FILENO);
		if(prep_ok && spam[i] < 0) {
			diag("for spam[%d], errno=%d", i, errno);
			prep_ok = false;
		}
	}
	ok1(prep_ok);

#ifdef __sneks__
	todo_start("unimplemented");
#endif

	int testfd = (*gen->fn)(gen->param), err = errno;
	ok1(testfd >= 0);
	for(int i=0; i < ARRAY_SIZE(spam); i++) {
		if(spam[i] >= 0) close(spam[i]);
	}
	if(testfd < 0) {
		skip(10, "generator failed, errno=%d", err);
		return;
	}

	int flags = fcntl(testfd, F_GETFD);
	if(!ok(flags >= 0, "fcntl F_GETFD")) diag("errno=%d", errno);
	iff_ok1(flags & FD_CLOEXEC, gen->sets_cloexec);
	skip_start(!invert, 3, "no invert") {
		int n = fcntl(testfd, F_SETFD,
			gen->sets_cloexec ? (flags & ~FD_CLOEXEC) : (flags | FD_CLOEXEC));
		ok(n == 0, "fcntl F_SETFD");
		/* confirm */
		flags = fcntl(testfd, F_GETFD);
		ok(flags >= 0, "fcntl F_GETFD");
		iff_ok1(flags & FD_CLOEXEC, !gen->sets_cloexec);
	} skip_end;

	int child = fork();
	if(child == 0) {
		char fdstr[80];
		snprintf(fdstr, sizeof fdstr, "%d", testfd);
		int n = setenv("TESTFD", fdstr, 1);
		if(n != 0) {
			diag("setenv failed, errno=%d", errno);
			exit(0xff);
		}
#ifdef __sneks__
		struct fd_bits *bits = __fdbits(testfd);
		assert(bits != NULL);
		snprintf(fdstr, sizeof fdstr, "%#lx:%#lx,%#x",
			L4_ThreadNo(bits->server), L4_Version(bits->server), bits->handle);
		n = setenv("TESTSNEKSIOHANDLE", fdstr, 1);
		if(n != 0) {
			diag("second setenv failed, errno=%d", errno);
			exit(0xfe);
		}
#endif
		execl(TESTDIR "/user/test/tools/fdcloser", "fdcloser", (char *)NULL);
		diag("execl(2) failed, errno=%d", errno);
		exit(0xfd);
	}
	int st, dead = waitpid(child, &st, 0);
	skip_start(!ok1(dead == child), 4, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		skip_start(!ok1(WEXITSTATUS(st) & 0x40), 2, "exit status is %d", WEXITSTATUS(st)) {
			bool needfail = !!(gen->sets_cloexec ^ invert);
			if(!ok(!!(WEXITSTATUS(st) & 1) == !needfail, "fdcloser's close(2)")) {
				diag("expected to %swork", needfail ? "not " : "");
			}
#ifndef __sneks__
			skip(1, "sneks only");
#else
			if(!ok(!!(WEXITSTATUS(st) & 2) == !needfail, "fdcloser's Sneks::IO/dup")) {
				diag("expected to %swork", needfail ? "not " : "");
			}
#endif
		} skip_end;
	} skip_end;

	close(testfd);
}
END_TEST

DECLARE_TEST("process:exec", cloexec);


static int gen_open(void *param) { return open(param, O_RDONLY); }
static int gen_open_c(void *param) { return open(param, O_RDONLY | O_CLOEXEC); }
static int gen_openat(void *param) { return openat(AT_FDCWD, param, O_RDONLY); }
static int gen_openat_c(void *param) { return openat(AT_FDCWD, param, O_RDONLY | O_CLOEXEC); }

static int gen_pipe(void *param) {
	int fds[2], n = pipe(fds);
	if(n >= 0) { close(fds[0]); n = fds[1]; }
	return n;
}

static int gen_pipe2(void *param) {
	int fds[2], n = pipe2(fds, param == NULL ? 0 : O_CLOEXEC);
	if(n >= 0) { close(fds[0]); n = fds[1]; }
	return n;
}

static int gen_dup(void *param) {
	int n = gen_pipe(NULL);
	if(n >= 0) { int fd = dup(n); close(n); n = fd; }
	return n;
}

static int gen_dup2(void *param) {
	int n = gen_pipe(NULL);
	if(n >= 0) {
		int othfd = gen_pipe(NULL);
		if(othfd < 0) { int err = errno; close(n); errno = err; return -1; }
		int fd = dup2(n, othfd);
		close(n);
		n = fd;
	}
	return n;
}

static int gen_dup3(void *param) {
	int n = gen_pipe(NULL);
	if(n >= 0) {
		int othfd = gen_pipe(NULL);
		if(othfd < 0) { int err = errno; close(n); errno = err; return -1; }
		int fd = dup3(n, othfd, param == NULL ? 0 : O_CLOEXEC), err = errno;
		close(n);
		if(fd < 0) close(othfd);
		errno = err;
		n = fd;
	}
	return n;
}
