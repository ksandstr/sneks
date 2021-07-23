
/* tests of POSIX syscalls related to symbolic links.
 *
 * TODO:
 *   - error cases:
 *     - non-positive bufsiz
 *     - symlink-specific failure modes of readlink: pathspec refers to a
 *       non-symlink object.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ccan/str/str.h>

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

#ifdef __sneks__
	todo_start("not implemented");
#endif

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
	linkbuf[PATH_MAX] = '\0';
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
