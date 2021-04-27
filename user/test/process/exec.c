
/* tests on the execve(2) family. */

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>

#include <sneks/test.h>


/* foundation: that an assistant program's exit status can be examined. */
START_LOOP_TEST(exit_status, iter, 0, 2)
{
	const char *prgname;
	switch(iter) {
		case 0: prgname = "exit_with_0"; break;
		case 1: prgname = "exit_with_1"; break;
		case 2: prgname = "exit_with_getpid"; break;
		default: assert(false);
	}
	diag("prgname=`%s'", prgname);
	plan_tests(6);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

#ifdef __sneks__
	todo_start("no implementation");
#endif

	int child = fork();
	if(child == 0) {
		char *fullpath = talloc_asprintf(NULL, "user/test/tools/%s", prgname);
		execl(fullpath, prgname, (char *)NULL);
		talloc_free(fullpath);
		int err = errno;
		diag("execl failed, errno=%d", err);
		exit(err ^ (child & 0xff));
	}
	diag("child=%d", child);

	int st, n = waitpid(child, &st, 0);
	skip_start(!ok(n == child, "waitpid"), 4, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		diag("WEXITSTATUS(st)=%d", WEXITSTATUS(st));
		imply_ok1(strends(prgname, "with_0"), WEXITSTATUS(st) == 0);
		imply_ok1(strends(prgname, "with_1"), WEXITSTATUS(st) == 1);
		imply_ok1(strends(prgname, "with_getpid"),
			WEXITSTATUS(st) == (child & 0xff));
	} skip_end;
}
END_TEST

DECLARE_TEST("process:exec", exit_status);


/* kinds of executable, positive: regular programs, scripts run by a regular
 * program, and scripts run by another script.
 */
START_LOOP_TEST(kinds_positive, iter, 0, 2)
{
	const char *prgname;
	switch(iter) {
		case 0: prgname = "tools/exit_with_0"; break;
		case 1: prgname = "scripts/first_order_script"; break;
		case 2: prgname = "scripts/second_order_script"; break;
		default: assert(false);
	}
	diag("prgname=`%s'", prgname);
	plan_tests(4);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

#ifdef __sneks__
	todo_start("no implementation");
#endif

	int child = fork();
	if(child == 0) {
		char *fullpath = talloc_asprintf(NULL, "user/test/%s", prgname);
		execl(fullpath, prgname, (char *)NULL);
		talloc_free(fullpath);
		int err = errno;
		diag("execl failed, errno=%d", err);
		exit(err | 0x80);
	}
	diag("child=%d", child);

	int st, n = waitpid(child, &st, 0);
	skip_start(!ok(n == child, "waitpid"), 2, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		ok1(WEXITSTATUS(st) == 0);
	} skip_end;
}
END_TEST

DECLARE_TEST("process:exec", kinds_positive);
