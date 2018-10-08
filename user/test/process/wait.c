
/* tests on the wait(2), waitpid(2), and waitid(2) syscalls. */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include <l4/ipc.h>
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
			if(active_exit) L4_Sleep(L4_TimePeriod(2 * 1000));
			exit(rc);
		}
		if(!active_exit) L4_Sleep(L4_TimePeriod(2 * 1000));
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
