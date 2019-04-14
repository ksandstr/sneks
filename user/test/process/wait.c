
/* tests on the wait(2), waitpid(2), and waitid(2) syscalls. */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include <sneks/test.h>


/* a call to wait(2) should return -1 (ECHILD) when there are no children. */
START_TEST(empty_wait)
{
	plan_tests(2);

	int st, pid = wait(&st);
	ok1(pid == -1);
	ok1(errno == ECHILD);
}
END_TEST

DECLARE_TEST("process:wait", empty_wait);


/* a call to waitpid(-1, &st, WHOHANG) should return -1 (ECHILD) when there
 * are children but they've not entered a waitable state.
 */
START_TEST(busy_wait)
{
	plan_tests(2);

	int child = fork();
	if(child == 0) {
		usleep(5 * 1000);
		exit(0);
	}

	int st, dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead < 0 && errno == ECHILD, "child hasn't exited")) {
		diag("dead=%d, st=%d, errno=%d", dead, st, errno);
	}
	usleep(10 * 1000);
	dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead == child, "child exited after sleep")) {
		dead = wait(&st);
		diag("waited on dead=%d (child=%d)", dead, child);
	}
}
END_TEST

DECLARE_TEST("process:wait", busy_wait);


/* check that return values are returned correctly. */
START_LOOP_TEST(return_value, iter, 0, 1)
{
	const bool active_exit = (iter & 1) != 0;
	plan_tests(2);

	bool wait_ok = true, rc_ok = true;
	for(int i=0; i < 20; i++) {
		int rc = i >= 10 ? i - 20 : i;
		int child = fork();
		if(child == 0) {
			if(active_exit) usleep(2 * 1000);
			exit(rc);
		}
		if(!active_exit) usleep(2 * 1000);
		int st, pid = wait(&st);
		if(pid < 0) {
			diag("wait(2) failed, errno=%d", errno);
			wait_ok = false;
			break;
		}
		if(!WIFEXITED(st)) {
			diag("i=%d, didn't exit?", i);
			rc_ok = false;
		} else if(WEXITSTATUS(st) != rc) {
			diag("i=%d, status=%d but rc=%d?", i, WEXITSTATUS(st), rc);
			rc_ok = false;
		}
	}

	ok1(wait_ok);
	ok1(rc_ok);
}
END_TEST

DECLARE_TEST("process:wait", return_value);
