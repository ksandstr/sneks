
/* tests of POSIX syscalls related to symbolic links.
 *
 * TODO:
 *   - error cases:
 *     - non-positive bufsiz
 *     - symlink-specific failure modes of readlink: pathspec refers to a
 *       non-symlink object.
 *   - move this into user/test/path/, tests into path:symlink, and bits into
 *     /user/test/path/symlink on the initrd
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>


/* positive function test of readlink(2) and readlinkat(2). does not cover the
 * Linux-specific O_PATH | O_NOFOLLOW dirfd mode.
 */
START_LOOP_TEST(readlink, iter, 0, 2)
{
	const char *method;
	switch(iter) {
		case 0: method = "readlink"; break;
		case 1: method = "readlinkat(AT_FDCWD, ...)"; break;
		case 2: method = "readlinkat(dirfd, ...)"; break;
		default: abort();
	}
	diag("method=`%s'", method);
	plan_tests(5);

	const char *dirbase = TESTDIR "/user/test/io";
	int dirfd = open(dirbase, O_DIRECTORY | O_RDONLY);
	if(!ok1(dirfd >= 0)) diag("errno=%d", errno);

	skip_start(iter == 2, 1, "not using cwd-relative pathspec") {
		int n = chdir(dirbase);
		ok(n == 0, "chdir");
	} skip_end;

	const char *linkpath = "symlink/teh_linkz0r";
	char *linkbuf = calloc(PATH_MAX + 1, 1);
	int n = -1; errno = -666;
	switch(iter) {
		case 0: n = readlink(linkpath, linkbuf, PATH_MAX); break;
		case 1: n = readlinkat(AT_FDCWD, linkpath, linkbuf, PATH_MAX); break;
		case 2: n = readlinkat(dirfd, linkpath, linkbuf, PATH_MAX); break;
	}
	if(n >= 0) linkbuf[n] = '\0';
	if(!ok(n > 0, "readlink call")) diag("n=%d, errno=%d", n, errno);
	if(!ok1(streq(linkbuf, "foo/bar/zot"))) diag("linkbuf=`%s'", linkbuf);
	if(!ok1(n == strlen(linkbuf))) {
		diag("n=%d, strlen(linkbuf)=%d", n, strlen(linkbuf));
	}

	close(dirfd);
	free(linkbuf);
}
END_TEST

DECLARE_TEST("io:symlink", readlink);


/* check that readlink()'s result is not null-terminated, and that results
 * longer than the provided buffer are also not null-terminated.
 *
 * TODO: proper testing of this function requires a working lstat() to
 * have a second source for the length of a symlink's contents.
 */
START_LOOP_TEST(unterminated_readlink_result, iter, 0, 1)
{
	const char *linkpath = TESTDIR "/user/test/io/symlink/teh_linkz0r";
	const int linksize = 11;
	diag("linkpath=`%s', linksize=%d", linkpath, linksize);
	const bool truncate = !!(iter & 1);
	diag("truncate=%s", btos(truncate));
	plan_tests(5);

	char buf[linksize + 16];
	memset(buf, '\x7f', sizeof buf);
	ssize_t n = readlink(linkpath, buf, truncate ? linksize - 5 : sizeof buf);
	int err = errno;
	skip_start(!ok(n >= 0, "readlink(2)"), 4, "err=%d", err) {
		ok(buf[n] == '\x7f', "didn't write past returned length");
		imply_ok1(n > 0, buf[n - 1] != '\0');
		imply_ok1(!truncate, buf[10] == 't');
		imply_ok1(truncate, buf[10] == '\x7f');
	} skip_end;
}
END_TEST

DECLARE_TEST("io:symlink", unterminated_readlink_result);


/* test symlink dereferencing in positive cases. variables:
 *   - whether symlink is terminal or non-terminal path component
 *   - whether symlink causes another symlink to be dereferenced
 *   - whether symlink is absolute or relative
 */
START_LOOP_TEST(deref_positive, iter, 0, 7)
{
	const bool terminal = !!(iter & 1), absolute = !!(iter & 2),
		iterated = !!(iter & 4);
	diag("terminal=%s, absolute=%s, iterated=%s",
		btos(terminal), btos(absolute), btos(iterated));
	plan_tests(5);

	char pathspec[256];
	snprintf(pathspec, sizeof pathspec,
		TESTDIR "/user/test/io/symlink/%sterminal%s%s%s",
		terminal ? "" : "non", iterated ? "_iterated" : "",
		absolute ? "_absolute" : "_relative",
		terminal ? "" : "/testfile");
	diag("pathspec=`%s'", pathspec);

	/* self-verification */
	char linkbuf[100];
	int n = readlink(pathspec, linkbuf, sizeof linkbuf - 1);
	if(n >= 0) linkbuf[n] = '\0';
	imply_ok1(!terminal, n < 0 && errno == EINVAL);
	imply_ok1(terminal, n > 0);
	skip_start(!terminal, 1, "readlink errno=%d", errno) {
		if(!iff_ok1(absolute, linkbuf[0] == '/')) diag("linkbuf=`%s'", linkbuf);
	} skip_end;

	/* actual test */
	struct stat st = { };
	if(!ok(stat(pathspec, &st) == 0, "stat call")) diag("errno=%d", errno);
	ok1((st.st_mode & S_IFMT) == S_IFREG);
}
END_TEST

DECLARE_TEST("io:symlink", deref_positive);


/* test symlink dereferencing in the case where a symlink leads to a
 * neverending loop, either as a terminal element or a non-terminal one.
 */
START_TEST(deref_loop)
{
	static const char *const cases[] = {
		TESTDIR "/user/test/io/symlink/looping_terminal",
		TESTDIR "/user/test/io/symlink/looping_middle/whatever/else",
	};
	plan_tests(ARRAY_SIZE(cases));

	for(int i=0; i < ARRAY_SIZE(cases); i++) {
		char *namepart = strstr(cases[i], "looping_"), *slash = strchr(namepart, '/');
		if(slash != NULL) {
			int len = slash - namepart;
			assert(namepart[len] == '/');
			char *copy = alloca(len + 1);
			memcpy(copy, namepart, len);
			copy[len] = '\0';
			namepart = copy;
		}
		errno = 0;
		if(!ok(stat(cases[i], &(struct stat){ }) < 0 && errno == ELOOP, "ELOOP for %s", namepart)) {
			diag("errno=%d", errno);
		}
	}
}
END_TEST

DECLARE_TEST("io:symlink", deref_loop);
