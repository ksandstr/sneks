
/* tests of POSIX syscalls related to symbolic links.
 *
 * TODO:
 *   - error cases:
 *     - non-positive bufsiz
 *     - symlink-specific failure modes of readlink: pathspec refers to a
 *       non-symlink object.
 */

#include <stdlib.h>
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

#ifdef __sneks__
	todo_start("unimplemented");
#endif

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
