
/* tests on the wait(2), waitpid(2), and waitid(2) syscalls. */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <sneks/test.h>

#ifdef __sneks__
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <sneks/sysinfo.h>
#endif


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

		abort();
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

		abort();
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


static void nothing_fn(int signo) {
	/* 'naff */
}


/* test whether waitpid(2) is idempotent when interrupted.
 *
 * specifically this is intended to hit the L4.X2 foible where an IPC "call"
 * can be interrupted, or time out, while its receive half is waiting on the
 * server. if this happens the server must roll back (or not complete lazily)
 * side effects of that call.
 *
 * for Proc::wait, we mean to provoke the case where the sleeper child's exit
 * causes a reply to a sleeping waiter, which fails, and so the zombie-reaping
 * should not occur. we observe this by a subsequent waitpid(2) succeeding,
 * where it'd pop ECHILD if the effect occurred unannounced.
 *
 * variables:
 *   - [slow_exit] sleeper sleeps for an additional 15ms between sigsuspend
 *     and exit.
 */
START_LOOP_TEST(lost_waitpid_on_interrupt, iter, 0, 1)
{
	const bool slow_exit = !!(iter & 1);
	diag("slow_exit=%s", btos(slow_exit));
	plan_tests(5);

	sigset_t olds, usr1;
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	int n = sigprocmask(SIG_BLOCK, &usr1, &olds);
	fail_unless(n == 0, "sigprocmask n=%d, errno=%d", n, errno);

	struct sigaction act = { .sa_handler = &nothing_fn };
	n = sigaction(SIGUSR1, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);

	int parent = getpid();
	int sleeper = fork_subtest_start("sleeping child") {
		plan(1);
		n = sigsuspend(&olds);
		ok(n == -1 && errno == EINTR, "sigsuspend");
		if(slow_exit) usleep(15 * 1000);
	} fork_subtest_end;

	int interloper = fork_subtest_start("interloper") {
		plan(1);
		usleep(8 * 1000);
		n = kill(parent, SIGUSR1);
		ok(n == 0, "kill(parent, SIGUSR1)");
	} fork_subtest_end;

	n = sigprocmask(SIG_SETMASK, &olds, NULL);
	fail_unless(n == 0, "sigprocmask n=%d, errno=%d", n, errno);
	int st;
	n = waitpid(sleeper, &st, 0);
	if(!ok1(n < 0 && errno == EINTR)) {
		diag("waitpid: n=%d, errno=%d", n, errno);
	}

	n = kill(sleeper, SIGUSR1);
	ok(n == 0, "kill sleeper");
	usleep(8 * 1000);

	fork_subtest_ok1(sleeper);
	fork_subtest_ok1(interloper);
}
END_TEST

DECLARE_TEST("process:wait", lost_waitpid_on_interrupt);


/* test that Proc::wait is idempotent under reply failure. */
START_LOOP_TEST(failing_wait, iter, 0, 1)
{
#ifndef __l4x2__
	plan_skip_all("depends on L4.X2 IPC semantics (iter=%d)", iter);
#else
	const bool failure = !!(iter & 1);
	diag("failure=%s", btos(failure));
	plan_tests(3);
	todo_start("borked");

	int child = fork();
	if(child == 0) exit(0);
	usleep(8 * 1000);

	/* fuck with UAPI a little bit first by sending it a Proc::wait with no
	 * intention of picking the reply up.
	 */
	skip_start(!failure, 1, "no fail") {
		struct __sysinfo *sip = __get_sysinfo(L4_GetKernelInterface());
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 4, .X.label = 0xe801 }.raw);
		L4_LoadMR(1, 0x1235);
		L4_LoadMR(2, P_PID);	/* idtype */
		L4_LoadMR(3, child);	/* id */
		L4_LoadMR(4, WNOHANG);	/* options */
		L4_MsgTag_t tag = L4_Send(sip->api.proc);
		if(!ok1(L4_IpcSucceeded(tag))) diag("ec=%#lx", L4_ErrorCode());
	} skip_end;

	/* with and without fuckery, this should succeed immediately due to
	 * the usleep() call up there.
	 */
	int st, n = waitpid(child, &st, WNOHANG);
	if(!ok1(n == child)) diag("n=%d, errno=%d", n, errno);
	ok1(WIFEXITED(st) && WEXITSTATUS(st) == 0);

	/* (clean up.) */
	n = waitpid(-1, &st, 0);
	if(n != -1 || errno != ECHILD) {
		diag("unexpected cleanup status, n=%d, errno=%d", n, errno);
	}
#endif
}
END_TEST

DECLARE_TEST("process:wait", failing_wait);
