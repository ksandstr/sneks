/* tests on process exit: raising SIGKILL, calling exit(), and calling _Exit(),
 * and atexit().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/str/str.h>

#include <sneks/test.h>


START_TEST(raise_sigkill)
{
	plan_tests(4);

	int child = fork();
	if(child == 0) {
		raise(SIGKILL);
		exit(2);
	}
	int st, dead = waitpid(child, &st, 0);
	ok1(dead == child);
	ok1(!WIFEXITED(st));
	ok1(WIFSIGNALED(st));
	ok1(WTERMSIG(st) == SIGKILL);
}
END_TEST

DECLARE_TEST("process:exit", raise_sigkill);


static void hello_printer(void);

START_LOOP_TEST(atexit_and_forced_exit, iter, 0, 1)
{
	const bool forced = !!(iter & 1);
	diag("forced=%s", btos(forced));
	plan_tests(3);

	int fds[2], n = pipe(fds);
	fail_unless(n == 0, "pipe(2): errno=%d", errno);

	int child = fork();
	if(child == 0) {
		n = dup2(fds[1], 1);
		if(n < 0) {
			diag("child dup2() failed, errno=%d", errno);
			exit(EXIT_FAILURE);
		}
		close(fds[0]);
		close(fds[1]);
		atexit(&hello_printer);
		if(!forced) exit(EXIT_SUCCESS); else _Exit(EXIT_SUCCESS);
		hello_printer();
	}
	close(fds[1]);
	FILE *input = fdopen(fds[0], "r");
	char line[100] = "";
	while(fgets(line, sizeof line, input) != NULL) {
		while(line[strlen(line) - 1] == '\n') line[strlen(line) - 1] = '\0';
		diag("line=`%s'", line);
	}
	fclose(input);
	int st, dead = waitpid(child, &st, 0);
	fail_unless(dead == child);
	ok1(WIFEXITED(st));
	ok1(WEXITSTATUS(st) == 0);
	iff_ok1(!forced, streq(line, "Hello, world!"));
}
END_TEST

DECLARE_TEST("process:exit", atexit_and_forced_exit);

static void hello_printer(void) {
	printf("Hello, world!\n");
}
