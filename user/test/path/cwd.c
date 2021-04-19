
/* tests on chdir(2) and fchdir(2). */

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>


/* run a subtest either forked or in the same process. */

#define run_subtest(forking, test_fn, param, fmt, ...) \
	do { \
		if((forking)) { \
			int _c = fork_subtest_start(fmt, ##__VA_ARGS__) { \
				(*(test_fn))((param)); \
			} fork_subtest_end; \
			fork_subtest_ok1(_c); \
		} else { \
			subtest_start((fmt), ##__VA_ARGS__); \
			(*(test_fn))((param)); \
			subtest_end(); \
		} \
	} while(false)


static const char test_directory[] = TESTDIR "/user/test";


static int chdir_via_open(const char *pathname)
{
	int dirfd = open(pathname, O_RDONLY | O_DIRECTORY);
	if(dirfd < 0) return -1;
	int n = fchdir(dirfd);
	close(dirfd);
	return n;
}


/* very basic chdir() and fchdir(). */
START_LOOP_TEST(chdir_basic, iter, 0, 1)
{
	const bool use_fchdir = !!(iter & 1);
	diag("use_fchdir=%s", btos(use_fchdir));

	static const char *args[] = { ".", "..", "test", "io", "/", ".", ".." };
	plan_tests(1 + ARRAY_SIZE(args));

	int (*chfn)(const char *) = use_fchdir ? &chdir_via_open : &chdir;

#ifdef __sneks__
	todo_start("impls missing");
#endif

	if(!ok((*chfn)(test_directory) == 0, "ch to testdir")) {
		diag("errno=%d", errno);
	}

	for(int i=0; i < ARRAY_SIZE(args); i++) {
		if(!ok((*chfn)(args[i]) == 0, "chdir to `%s'", args[i])) {
			diag("errno=%d", errno);
		}
	}
}
END_TEST

DECLARE_TEST("path:cwd", chdir_basic);


/* test that chdir() and fchdir() change what unqualified open() sees. */

static void try_open_dir(const char *path);


START_LOOP_TEST(chdir_effect, iter, 0, 3)
{
	const bool use_fchdir = !!(iter & 1), forking = !!(iter & 2);
	diag("use_fchdir=%s, forking=%s", btos(use_fchdir), btos(forking));
	plan_tests(4);

	int (*chfn)(const char *) = use_fchdir ? &chdir_via_open : &chdir;

#ifdef __sneks__
	todo_start("impls missing");
#endif

	ok(chdir(test_directory) == 0, "setup");

	int fd = open("test", O_DIRECTORY | O_RDONLY);
	ok(fd < 0 && errno == ENOENT, "first open fails");
	if(fd >= 0) close(fd);

	if(!ok1((*chfn)("..") == 0)) diag("errno=%d", errno);
	run_subtest(forking, try_open_dir, "test", "second open succeeds");
}
END_TEST

DECLARE_TEST("path:cwd", chdir_effect);


static void try_open_dir(const char *path) {
	plan(1);
	int fd = open(path, O_DIRECTORY | O_RDONLY);
	ok(fd >= 0, "try_open(`%s')", path);
	if(fd >= 0) close(fd);
}


/* try to change into a file, which shouldn't work */

static void try_chdir_reg(int (*chfn)(const char *));


START_LOOP_TEST(chdir_fails, iter, 0, 3)
{
	const bool use_fchdir = !!(iter & 1), forking = !!(iter & 2);
	diag("use_fchdir=%s, forking=%s", btos(use_fchdir), btos(forking));
	plan_tests(2);

	int (*chfn)(const char *) = use_fchdir ? &chdir_via_open : &chdir;

#ifdef __sneks__
	todo_start("impls missing");
#endif

	ok(chdir(test_directory) == 0, "setup");
	run_subtest(forking, try_chdir_reg, chfn, "chdir into regular file");
}
END_TEST

DECLARE_TEST("path:cwd", chdir_fails);


static void try_chdir_reg(int (*chfn)(const char *))
{
	plan(2);
	ok1((*chfn)("io/dir/0") < 0 && errno == ENOTDIR);
	ok1((*chfn)("io/dir/1/whateverest") < 0 && errno == ENOTDIR);
}
