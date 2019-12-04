
/* tests on the wait(2), waitpid(2), and waitid(2) syscalls. */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
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
	if(!ok(dead == 0, "child hasn't exited")) {
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
		} else if((int8_t)WEXITSTATUS(st) != rc) {
			diag("i=%d, status=%d but rc=%d?", i, WEXITSTATUS(st), rc);
			rc_ok = false;
		}
	}

	ok1(wait_ok);
	ok1(rc_ok);
}
END_TEST

DECLARE_TEST("process:wait", return_value);


/* check that a child process calling abort() breaks and returns
 * WTERMSIG=SIGABRT under default signal disposition.
 *
 * variables:
 *   - block SIGABRT or not.
 */
START_LOOP_TEST(aborting_child, iter, 0, 1)
{
	const bool block_abrt = !!(iter & 1);
	diag("block_abrt=%s", btos(block_abrt));
	plan_tests(2);
#ifdef __sneks__
	todo_start("woop shoop");
#endif

	int child = fork();
	fail_if(child < 0);
	if(child == 0) {
		struct sigaction act = { .sa_handler = SIG_DFL };
		int n = sigaction(SIGABRT, &act, NULL);
		if(n < 0) diag("child: sigaction failed, errno=%d", errno);

		sigset_t abrt_set;
		sigemptyset(&abrt_set);
		sigaddset(&abrt_set, SIGABRT);
		n = sigprocmask(block_abrt ? SIG_BLOCK : SIG_UNBLOCK,
			&abrt_set, NULL);
		if(n < 0) diag("child: sigprocmask failed, errno=%d", errno);

#ifndef __sneks__
		abort();
#else
		exit(0);
#endif
	}
	int st, dead = waitpid(child, &st, 0);
	fail_if(dead != child);

	ok1(WIFSIGNALED(st));
	imply_ok1(WIFSIGNALED(st), WTERMSIG(st) == SIGABRT);
}
END_TEST

DECLARE_TEST("process:wait", aborting_child);


static jmp_buf on_abrt_jmp;
static bool longjmp_out;

static void sigabrt_fn(int signum) {
	if(longjmp_out) longjmp(on_abrt_jmp, 1);	/* yump */
}


/* handle SIGABRT within an aborting child to see what happens.
 * variables:
 *   - catching or ignoring
 *   - longjmp out or not
 */
START_LOOP_TEST(catch_or_ignore_aborting_child, iter, 0, 3)
{
	const bool catching = !!(iter & 1);
	longjmp_out = !!(iter & 2);
	diag("catching=%s, longjmp_out=%s", btos(catching), btos(longjmp_out));
	plan_tests(2);
#ifdef __sneks__
	todo_start("not gonna work this way");
#endif

	int child = fork();
	fail_if(child < 0);
	if(child == 0) {
		if(setjmp(on_abrt_jmp)) {
			diag("child in positive setjmp clause");
			exit(2);
		}

		struct sigaction act = {
			.sa_handler = catching ? &sigabrt_fn : SIG_IGN,
		};
		int n = sigaction(SIGABRT, &act, NULL);
		if(n < 0) diag("child: sigaction failed, errno=%d", errno);

#ifndef __sneks__
		abort();
#else
		exit(0);
#endif
	}

	int st, dead = waitpid(child, &st, 0);
	fail_if(dead < 0);
	diag("st: %s, val=%d",
		WIFEXITED(st) ? "exited" : (WIFSIGNALED(st) ? "signaled" : "other"),
		WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? WTERMSIG(st) : 0));

	imply_ok1(catching && longjmp_out,
		WIFEXITED(st) && WEXITSTATUS(st) == 2);
	imply_ok1(!catching || !longjmp_out,
		WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
}
END_TEST

DECLARE_TEST("process:wait", catch_or_ignore_aborting_child);
