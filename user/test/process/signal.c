
/* tests on POSIX signaling: sigaction, kill, and so forth.
 *
 * NOTE: most of these tests should be also compiled for the host system and
 * executed to determine that they can return all green on a mature,
 * well-supported platform. that'll require a L4.X2 adaptation layer for e.g.
 * thread ID checks and L4_Sleep(), but that's simple enough.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ccan/minmax/minmax.h>

#ifdef __l4x2__
#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/syscall.h>
#endif

#include <sneks/test.h>


static sig_atomic_t chld_got = 0, int_got = 0, int_max_depth = 0;

#ifdef __l4x2__
static L4_ThreadId_t chld_handler_tid = { .raw = 0 },
	int_handler_tid = { .raw = 0 };
#endif


static void basic_sigchld_handler(int signum)
{
#ifdef __l4x2__
	chld_handler_tid = L4_MyGlobalId();
#endif
	for(;;) {
		int st, dead = waitpid(-1, &st, WNOHANG);
		if(dead <= 0) break;
		chld_got++;
	}
}


START_LOOP_TEST(sigaction_basic, iter, 0, 1)
{
	const bool sleep_in_recv = !!(iter & 1);
	diag("sleep_in_recv=%s", btos(sleep_in_recv));
	plan_tests(4);

	chld_got = 0;
	struct sigaction act = { .sa_handler = &basic_sigchld_handler };
	int n = sigaction(SIGCHLD, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);
	int child = fork();
	if(child == 0) {
		/* ensure that the parent gets to a sleep.
		 *
		 * TODO: the no-sleep case is contained in the uninterruptible receive
		 * case, but should be provoked explicitly as well.
		 */
		usleep(2 * 1000);
		exit(0);
	}
	if(!ok(child > 0, "fork")) diag("errno=%d", errno);

	const int timeout_us = 5000;
	int iters = 5;
	while(child > 0 && chld_got < 1 && --iters) {
#ifndef __l4x2__
		if(usleep(timeout_us) < 0) {
			diag("usleep(3) failed, errno=%d (not an error)", errno);
		}
#else
		L4_Time_t iter_timeout = L4_TimePeriod(timeout_us);
		L4_MsgTag_t tag;
		L4_ThreadId_t dummy;
		if(sleep_in_recv) {
			tag = L4_Ipc(L4_nilthread, L4_Myself(),
				L4_Timeouts(L4_ZeroTime, iter_timeout), &dummy);
		} else {
			tag = L4_Ipc(L4_Myself(), L4_nilthread,
				L4_Timeouts(iter_timeout, L4_ZeroTime), &dummy);
		}
		if(L4_IpcFailed(tag) && (L4_ErrorCode() & ~1ul) != 2) {
			diag("sleep failed, ec=%lu (not an error)", L4_ErrorCode());
		}
#endif
	}
	if(!ok(iters > 0, "signal was processed") && child > 0) {
		int st, dead = wait(&st);
		diag("waited for dead=%d (child=%d)", dead, child);
	}
#ifdef __l4x2__
	ok(L4_SameThreads(L4_Myself(), chld_handler_tid),
		"current thread was used");
#else
	skip(1, "no emulation of L4_SameThreads() yet");
#endif
}
END_TEST

DECLARE_TEST("process:signal", sigaction_basic);



static void recur_sigchld_handler(int signum)
{
#ifdef __l4x2__
	chld_handler_tid = L4_MyGlobalId();
#endif
	kill(getpid(), SIGINT);
	for(;;) {
		int st, dead = waitpid(-1, &st, WNOHANG);
		if(dead <= 0) break;
		chld_got++;
	}
}


static void recur_sigint_handler(int signum)
{
	int_got++;
#ifdef __l4x2__
	int_handler_tid = L4_MyGlobalId();
#endif
}


/* test that other signals can be processed during the processing of a
 * different signal. sets up for SIGCHLD like sigaction_basic, then sends
 * SIGINT from within the signal handler.
 *
 * TODO: more variables:
 *   - once kill(2) works from child to parent, add a mode where the second
 *     signal comes from out of process instead.
 *   - and another where it comes from a different thread.
 */
START_TEST(sigaction_recur)
{
	plan_tests(7);

	chld_got = 0; int_got = 0;
	struct sigaction act = { .sa_handler = &recur_sigchld_handler };
	int n = sigaction(SIGCHLD, &act, NULL);
	if(!ok(n == 0, "sigaction for CHLD")) diag("errno=%d", errno);
	act.sa_handler = &recur_sigint_handler;
	n = sigaction(SIGINT, &act, NULL);
	if(!ok(n == 0, "sigaction for INT")) diag("errno=%d", errno);
	int child = fork();
	if(child == 0) {
		usleep(2 * 1000);
		exit(0);
	}
	if(!ok(child > 0, "fork")) diag("errno=%d", errno);

	const int timeout_us = 5000;
	int iters = 5;
	while(child > 0 && chld_got < 1 && --iters) {
#ifndef __l4x2__
		if(usleep(timeout_us) < 0) {
			diag("usleep(3) failed, errno=%d (not an error)", errno);
		}
#else
		const L4_Time_t iter_timeout = L4_TimePeriod(timeout_us);
		L4_ThreadId_t dummy;
		L4_MsgTag_t tag = L4_Ipc(L4_Myself(), L4_nilthread,
			L4_Timeouts(iter_timeout, L4_ZeroTime), &dummy);
		if(L4_IpcFailed(tag) && (L4_ErrorCode() & ~1ul) != 2) {
			diag("sleep failed, ec=%lu (not an error)", L4_ErrorCode());
		}
#endif
	}
	if(!ok(iters > 0, "signal was processed") && child > 0) {
		int st, dead = wait(&st);
		diag("waited for dead=%d (child=%d)", dead, child);
	}

	ok1(chld_got > 0);
	ok1(int_got > 0);
#ifdef __l4x2__
	ok(L4_SameThreads(L4_Myself(), int_handler_tid),
		"SIGINT handled in main thread");
#else
	skip(1, "no emulation of L4_SameThreads() yet");
#endif
}
END_TEST

DECLARE_TEST("process:signal", sigaction_recur);



static void defer_sigint_handler(int signum)
{
	static _Atomic int depth = 0;

	int new_depth = atomic_fetch_add(&depth, 1) + 1;
	int_max_depth = max_t(int, int_max_depth, new_depth);
	if(++int_got == 1) kill(getpid(), SIGINT);
	atomic_fetch_sub(&depth, 1);
}


/* test SA_NODEFER.
 *
 * TODO: pop SIGINT from external sources: a child process, or a different
 * thread.
 */
START_LOOP_TEST(sigaction_defer, iter, 0, 1)
{
	const bool set_nodefer = !!(iter & 1);
	diag("set_nodefer=%s", btos(set_nodefer));
	plan_tests(4);

	struct sigaction act = {
		.sa_handler = &defer_sigint_handler,
		.sa_flags = set_nodefer ? SA_NODEFER : 0,
	};
	int n = sigaction(SIGINT, &act, NULL);
	ok(n == 0, "sigaction");

	int_got = 0;
	int_max_depth = 0;
	if(n == 0) {
		kill(getpid(), SIGINT);
		diag("int_got=%d, int_max_depth=%d", int_got, int_max_depth);
	}

	ok1(int_got == 2);
	imply_ok1(!set_nodefer, int_max_depth == 1);
	imply_ok1(set_nodefer, int_max_depth == 2);
}
END_TEST

DECLARE_TEST("process:signal", sigaction_defer);


static void ignore_signal(int signum) {
	/* as it says on the tin */
}


/* start a subprocess and handshake with it only after a signal has been
 * delivered.
 */
START_TEST(pause_basic)
{
	plan_tests(2);

	int child = fork();
	if(child == 0) {
		struct sigaction act = { .sa_handler = &ignore_signal };
		int n = sigaction(SIGCHLD, &act, NULL);
		if(n != 0) {
			diag("child's sigaction(2) failed, errno=%d", errno);
			exit(1);
		}
		pause();
		exit(0);
	}

	usleep(5 * 1000);
	todo_start("sneks' waitpid() handles no-change wrong");
	int st, dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead == 0, "no waitpid before signal")) {
		diag("dead=%d, st=%d, errno=%d", dead, st, errno);
	}

	int n = kill(child, SIGCHLD);	/* FIXME: use something different */
	if(n != 0) diag("kill(2) failed, errno=%d", errno);

	usleep(5 * 1000);
	dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead == child, "waitpid succeeds after signal")) {
		diag("dead=%d, st=%d, errno=%d", dead, st, errno);
		/* FIXME: clean the child up with SIGKILL or something. */
	}
}
END_TEST

DECLARE_TEST("process:signal", pause_basic);


static void ks_sigint_handler(int signum) {
	if(signum == SIGINT) int_got++;
}

/* what it says on the tin: kill yourself to see what happens. */
START_TEST(kill_self)
{
	plan_tests(3);

	int_got = 0;
	struct sigaction act = { .sa_handler = &ks_sigint_handler };
	int n = sigaction(SIGINT, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);

	n = kill(getpid(), SIGINT);
	if(!ok(n == 0, "kill")) diag("errno=%d", errno);

	ok1(int_got > 0);
}
END_TEST

DECLARE_TEST("process:signal", kill_self);


/* the most basic test of sigprocmask(2) and sigpending(2): does masking
 * SIGINT prevent its delivery? does it show up in sigpending(2)? does the
 * signal get delivered once unmasked?
 */
START_LOOP_TEST(procmask_and_pending_basic, iter, 0, 1)
{
	const bool unmask_by_set = !!(iter & 1);
	diag("unmask_by_set=%s", btos(unmask_by_set));
	plan_tests(9);

	int_got = 0;
	struct sigaction act = { .sa_handler = &ks_sigint_handler };
	int n = sigaction(SIGINT, &act, NULL);
	fail_unless(n == 0);

	/* first, unmask SIGINT and deliver it once. */
	sigset_t sigint_set;
	sigemptyset(&sigint_set);
	sigaddset(&sigint_set, SIGINT);
	n = sigprocmask(SIG_UNBLOCK, &sigint_set, NULL);
	if(!ok(n == 0, "unblock SIGINT")) {
		diag("errno=%d", errno);
	}
	int before = int_got;
	n = kill(getpid(), SIGINT);
	fail_unless(n == 0);
	ok(int_got == before + 1, "got unblocked SIGINT");
	sigset_t pend;
	sigemptyset(&pend);
	n = sigpending(&pend);
	ok(n == 0, "sigpending");
	ok1(!sigismember(&pend, SIGINT));

	/* now mask, stimulate, test, and unblock. */
	sigset_t oldset;
	sigemptyset(&oldset);
	n = sigprocmask(SIG_BLOCK, &sigint_set, &oldset);
	if(!ok(n == 0, "block SIGINT")) {
		diag("errno=%d", errno);
	}
	before = int_got;
	n = kill(getpid(), SIGINT);
	fail_unless(n == 0);
	ok(int_got == before, "did not get blocked SIGINT");

	sigemptyset(&pend);
	n = sigpending(&pend);
	ok1(sigismember(&pend, SIGINT));

	if(unmask_by_set) {
		sigprocmask(SIG_SETMASK, &oldset, NULL);
	} else {
		sigprocmask(SIG_UNBLOCK, &sigint_set, NULL);
	}
	ok(int_got == before + 1, "got delayed SIGINT");

	sigemptyset(&pend);
	sigpending(&pend);
	ok1(!sigismember(&pend, SIGINT));
}
END_TEST

DECLARE_TEST("process:signal", procmask_and_pending_basic);
