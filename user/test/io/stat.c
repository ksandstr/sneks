
/* tests on the stat(2) family: stat, fstat, lstat, fstatat.
 *
 * TODO:
 *   - tests on all functions across all result fields. currently only the
 *     S_IFMT bits of st_mode are covered, and those lack a few cases as well.
 *   - failure cases of all functions
 *   - stat, fstatat equivalent for fstat_type
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>


struct fd_type_case {
	int (*genfn)(void);
	int exp_ifmt;
	const char *name;
};

/* TODO: move this into a utility thing, because we can surely test for styles
 * of duplicate elsewhere also.
 */
struct dup_style_case {
	int (*dupfn)(int);
	const char *name;
};


static int gen_reg(void);
static int gen_dir(void);
static int gen_pipe(void);
static int gen_chrdev(void);

static int dup_none(int);
static int dup_dup(int);
static int dup_dup2(int);
static int dup_dup3(int);
static int dup_fcntl(int);


static const struct fd_type_case fd_types[] = {
	{ &gen_reg, S_IFREG, "regular file" },
	{ &gen_dir, S_IFDIR, "directory" },
	{ &gen_pipe, S_IFIFO, "pipe" },
	{ &gen_chrdev, S_IFCHR, "character device" },
	/* TODO: named pipe, socket, block device, epoll */
};

static const struct dup_style_case dup_styles[] = {
	{ &dup_none, "none" },
	{ &dup_dup, "dup" },
	{ &dup_dup2, "dup2" },
	{ &dup_dup3, "dup3" },
	{ &dup_fcntl, "fcntl/F_DUPFD" },
};


/* test that fstat() returns the correct S_IFMT bits in st_mode. */
START_LOOP_TEST(fstat_type, iter, 0,
	ARRAY_SIZE(fd_types) * ARRAY_SIZE(dup_styles) - 1)
{
	const struct fd_type_case *typ = &fd_types[iter % ARRAY_SIZE(fd_types)];
	const struct dup_style_case *dup = &dup_styles[iter / ARRAY_SIZE(fd_types)];
	diag("typ->name=`%s', dup->name=`%s'", typ->name, dup->name);
	plan_tests(4);

	int fd = (*typ->genfn)();
	skip_start(!ok(fd >= 0, "got fd"), 3, "genfn errno=%d", errno) {
		int dupe = (*dup->dupfn)(fd);
		skip_start(!ok(dupe >= 0, "got dupe"), 2, "dupfn errno=%d", errno) {
			struct stat st;
			int n = fstat(dupe, &st);
			skip_start(!ok(n == 0, "fstat(2)"), 1, "fstat(2) errno=%d", errno) {
				if(!ok1(typ->exp_ifmt == (st.st_mode & S_IFMT))) {
					diag("->exp_ifmt=%#x, got=%#x", typ->exp_ifmt, st.st_mode & S_IFMT);
				}
			} skip_end;
		} skip_end;
		if(dupe >= 0 && dupe != fd) close(dupe);
	} skip_end;

	close(fd);
}
END_TEST

DECLARE_TEST("io:stat", fstat_type);


static int gen_reg(void) {
	return open(TESTDIR "/user/test/io/dir/0", O_RDONLY);
}


static int gen_dir(void) {
	return open(TESTDIR "/user/test/io/dir", O_RDONLY | O_DIRECTORY);
}


static int gen_pipe(void) {
	int fds[2], n = pipe(fds);
	if(n < 0) return n;
	else {
		close(fds[0]);
		return fds[1];
	}
}


static int gen_chrdev(void) {
	return open("/dev/null", O_RDWR);
}


static int dup_none(int fd) { return fd; }

static int dup_dup(int fd) { return dup(fd); }

static int dup_dup2(int fd) {
	int newfd = gen_pipe();
	if(newfd < 0) return -1;
	int res = dup2(fd, newfd);
	if(res < 0) close(newfd);
	return res;
}

static int dup_dup3(int fd) {
	int newfd = gen_pipe();
	if(newfd < 0) return -1;
	int res = dup3(fd, newfd, 0);
	if(res < 0) close(newfd);
	return res;
}

static int dup_fcntl(int fd) { return fcntl(fd, F_DUPFD, 3); }


/* test that fstat() returns correct mode bits in at least one of the u/g/o
 * sets.
 */

static const struct {
	const char *path;
	int mode;
} mode_tests[] = {
	{ TESTDIR "/user/test/io/stat/r", S_IRUSR },
	{ TESTDIR "/user/test/io/stat/x", S_IXUSR },
	{ TESTDIR "/user/test/io/stat/rx", S_IRUSR | S_IXUSR },
	/* (writability can't be tested on squashfs) */
	//{ TESTDIR "/user/test/io/stat/w", S_IWUSR },
};


/* TODO: add O_EXEC to <fcntl.h> as another bit of O_ACCMODE, which must
 * expand.
 */
START_LOOP_TEST(fstat_mode, iter, 0, ARRAY_SIZE(mode_tests) - 1)
{
	const char *path = mode_tests[iter].path;
	const int exp_mode = mode_tests[iter].mode;
	diag("path=`%s', exp_mode=%#o", path, exp_mode);

	int o_mode = 0;
#ifndef O_EXEC
	if((exp_mode & S_IRWXU) == S_IXUSR) {
		plan_skip_all("no O_EXEC on target?");
		return;
	}
#else
	if(exp_mode & S_IXUSR) o_mode = O_EXEC;
#endif
	if(o_mode == 0 && (exp_mode & S_IRUSR)) o_mode = O_RDONLY;

	plan_tests(3);

	int fd = open(path, o_mode);
	skip_start(!ok(fd >= 0, "open(2)"), 2, "open errno=%d", errno) {
		struct stat st;
		int n = fstat(fd, &st);
		skip_start(!ok(n == 0, "fstat(2)"), 1, "fstat errno=%d", errno) {
			if(!ok1((st.st_mode & S_IRWXU) == exp_mode)) {
				diag("mode=%#o", st.st_mode & S_IRWXU);
			}
		} skip_end;
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stat", fstat_mode);


/* same, but for stat() and fstatat(). */
START_LOOP_TEST(stat_mode, iter, 0, ARRAY_SIZE(mode_tests) * 3 - 1)
{
	const char *path = mode_tests[iter % 3].path, *method;
	const int exp_mode = mode_tests[iter % 3].mode, which = iter / 3;
	switch(which) {
		case 0: method = "stat"; break;
		case 1: method = "fstat(AT_FDCWD, ...)"; break;
		case 2: method = "fstat(x, ...)"; break;
		default: abort();
	}
	diag("path=`%s', exp_mode=%#o, method=`%s'", path, exp_mode, method);
	plan_tests(3);

	struct stat st = { };
	int n = -1;
	if(which != 2) skip(1, "don't need dirfd");
	switch(which) {
		case 0: n = stat(path, &st); break;
		case 1: n = fstatat(AT_FDCWD, path, &st, 0); break;
		case 2: {
			int dirfd = open(TESTDIR "/user", O_RDONLY | O_DIRECTORY);
			if(ok1(dirfd >= 0)) {
				char relpath[strlen(path) - strlen(TESTDIR) + 16];
				snprintf(relpath, sizeof relpath, "../%s", path + strlen(TESTDIR) + 1);
				diag("relpath=`%s'", relpath);
				n = fstatat(dirfd, path, &st, 0);
				close(dirfd);
			}
			break;
		}
		default: abort();
	}

	skip_start(!ok(n >= 0, "stat-call"), 1, "errno=%d", errno) {
		if(!ok1((st.st_mode & S_IRWXU) == exp_mode)) {
			diag("mode=%#o", st.st_mode & S_IRWXU);
		}
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stat", stat_mode);


/* statting a symlink. */
START_LOOP_TEST(symlink_terminal, iter, 0, 3)
{
	const char *pathspec = TESTDIR "/user/test/io/stat/exploding_symlink";
	const bool follow = !!(iter & 1), use_fstatat = !!(iter & 2);
	diag("follow=%s, use_fstatat=%s", btos(follow), btos(use_fstatat));
	plan_tests(3);

	struct stat st = { };
	int n;
	if(!use_fstatat) {
		if(!follow) n = lstat(pathspec, &st); else n = stat(pathspec, &st);
	} else {
		n = fstatat(AT_FDCWD, pathspec, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
	}
	if(n < 0) diag("errno=%d", errno);

	imply_ok1(!follow, n == 0);
	imply_ok1(follow, n < 0 && errno == ENOENT);
	skip_start(n < 0, 1, "no entrails to see") {
		if(!ok1((st.st_mode & S_IFMT) == S_IFLNK)) diag("mode=%#o", st.st_mode);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stat", symlink_terminal);

/* statting a device node and fstatting an open descriptor of the same device
 * node should produce identical results.
 */
START_TEST(device_node)
{
	const char *testdev = "/dev/zero";
	diag("testdev=`%s'", testdev);
	plan_tests(9);
	struct stat pst = { };
	if(!ok(stat(testdev, &pst) == 0, "stat testdev path")) diag("stat failed: %s", strerror(errno));
	ok1(S_ISCHR(pst.st_mode));
	int fd = open(testdev, O_RDONLY);
	skip_start(!ok(fd >= 0, "open testdev path"), 6, "%s", strerror(errno)) {
		struct stat fst = { };
		if(!ok(fstat(fd, &fst) == 0, "fstat testdev fd")) diag("fstat failed: %s", strerror(errno));
		ok1(S_ISCHR(fst.st_mode));
		ok(memcmp(&pst, &fst, sizeof(struct stat)) == 0, "same bits");
		int fd2 = dup(fd);
		skip_start(!ok(fd2 >= 0, "dup testdev fd"), 2, "%s", strerror(errno)) {
			struct stat dupst = { };
			if(!ok(fstat(fd2, &dupst) == 0, "fstat fd2")) diag("fstat failed: %s", strerror(errno));
			ok(memcmp(&pst, &dupst, sizeof(struct stat)) == 0, "same bits");
			close(fd2);
		} skip_end;
		close(fd);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:stat", device_node);
