
/* tests on POSIX signaling:
 *   - portable interface: sigaction, kill, sigsuspend, sigprocmask, etc.
 *   - handler semantics: preservation of state (errno, L4.X2 vregs, etc.),
 *     longjmp and ucontext interactions, etc.
 *   - semantics of forbid/permit signals interrupting receive-wait IPC.
 *   - (etc.)
 *   - BUT NOT tests on preservation of signal handling state (procmask,
 *     handlers, etc.) across fork().
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ccan/minmax/minmax.h>

#ifdef __l4x2__
#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/syscall.h>
#include <l4/vregs.h>
#endif

#include <sneks/test.h>


/* assorted globals, semantics per handler */
static volatile sig_atomic_t chld_got = 0, int_got = 0,
	handler_calls = 0, int_max_depth = 0,
	n_sigs_handled = 0, sig_got = 0;


#ifdef __l4x2__
static volatile L4_ThreadId_t chld_handler_tid = { .raw = 0 },
	int_handler_tid = { .raw = 0 },
	sig_handler_tid;
#endif


static void sig_basic_handler_fn(int signum) {
	diag("%s: signum=%d", __func__, signum);
#ifdef __l4x2__
	sig_handler_tid = L4_MyGlobalId();
#endif
	sig_got++;
}


/* test basic sigaction(2) handling using both kinds of signal, sent from a
 * child process.
 *
 * variables:
 *   - [realtime] use a reliable/realtime signal
 */
START_LOOP_TEST(sigaction_basic, iter, 0, 1)
{
	const bool realtime = !!(iter & 1);
	diag("realtime=%s", btos(realtime));
	plan_tests(4);
	const int signum = realtime ? SIGRTMIN+17 : SIGUSR1;
	assert(signum <= SIGRTMAX);
	diag("signum=%d (rt=[%d..%d])", signum, SIGRTMIN, SIGRTMAX);

#ifdef __l4x2__
	sig_handler_tid = L4_nilthread;
#endif
	sig_got = 0;

	struct sigaction act = { .sa_handler = &sig_basic_handler_fn };
	int n = sigaction(signum, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);
	int parent = getpid(), child = fork_subtest_start("helper child") {
		plan(1);
		/* ensure that the parent gets to a sleep. */
		usleep(2 * 1000);
		n = kill(parent, signum);
		ok1(n == 0);
	} fork_subtest_end;

	const int timeout_us = 5000;
	int iters = 5;
	while(child > 0 && sig_got < 1 && --iters) {
		if(usleep(timeout_us) < 0) {
			diag("usleep(3) failed, errno=%d (not an error)", errno);
		}
	}
	ok(iters > 0, "signal was processed");
	fork_subtest_ok1(child);
#ifdef __l4x2__
	ok(L4_SameThreads(L4_Myself(), sig_handler_tid),
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


/* error returns from kill(2).
 * TODO: fill the rest in which e.g. kill_permissions doesn't cover.
 */
START_TEST(kill_error)
{
	plan_tests(2);

	ok1(kill(getpid(), -1) < 0 && errno == EINVAL);
	ok1(kill(getpid(), 12345) < 0 && errno == EINVAL);
}
END_TEST

DECLARE_TEST("process:signal", kill_error);


static void count_sigint_handler(int signum) {
	if(signum == SIGINT) int_got++;
	handler_calls++;
}

/* what it says on the tin: kill yourself to see what happens. */
START_TEST(kill_self)
{
	plan_tests(3);

	int_got = 0;
	struct sigaction act = { .sa_handler = &count_sigint_handler };
	int n = sigaction(SIGINT, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);

	n = kill(getpid(), SIGINT);
	if(!ok(n == 0, "kill")) diag("errno=%d", errno);

	ok1(int_got > 0);
}
END_TEST

DECLARE_TEST("process:signal", kill_self);


/* test for process existence with kill(pid, 0). */
START_TEST(kill_zero)
{
	plan_tests(4);

	ok1(kill(getpid(), 0) == 0);

	int child = fork();
	if(child == 0) {
		usleep(5 * 1000);
		exit(0);
	}
	ok1(kill(child, 0) == 0);
	int st, dead = waitpid(child, &st, 0);
	ok1(dead == child);
	ok1(kill(child, 0) < 0 && errno == ESRCH);
}
END_TEST

DECLARE_TEST("process:signal", kill_zero);


/* test that kill(2) is able to send to anyone if the caller is root, when
 * sender's real or effective uid matches recipient's real or saved uid, and
 * fails with EPERM otherwise.
 *
 * variables:
 *   - [from_root] whether signal is sent from uid 0.
 *   - [change_real, change_eff, change_saved] whether sender should have
 *     different UID in the respective field when sending the signal.
 *   - [rc_{real,eff,saved}] same for the receiver side
 */
START_LOOP_TEST(kill_permissions, iter, 0, 127)
{
	if(getuid() != 0) {
		plan_skip_all("root access required (for setuid)");
		return;
	}

	const bool from_root = !!(iter & 1),
		change_real = !!(iter & 2), change_eff = !!(iter & 4),
		change_saved = !!(iter & 8),
		rc_real = !!(iter & 16), rc_eff = !!(iter & 32),
		rc_saved = !!(iter & 64);
	diag("from_root=%s, change_real=%s, _eff=%s, _saved=%s", btos(from_root),
		btos(change_real), btos(change_eff), btos(change_saved));
	diag("  rc_real=%s, rc_eff=%s, rc_saved=%s",
		btos(rc_real), btos(rc_eff), btos(rc_saved));

	plan_tests(5);

	int_got = 0;
	struct sigaction act = { .sa_handler = &count_sigint_handler };
	int n = sigaction(SIGINT, &act, NULL);
	fail_unless(n == 0);

	sigset_t sigint_set, oldset;
	sigemptyset(&sigint_set);
	sigaddset(&sigint_set, SIGINT);
	n = sigprocmask(SIG_BLOCK, &sigint_set, &oldset);
	fail_unless(n == 0);

	int receiver = fork_subtest_start("signal receiver") {
		plan_tests(2);
		n = setresuid(rc_real ? 2222 : 1000, rc_eff ? 2222 : 1000,
			rc_saved ? 2222 : 1000);
		if(!ok1(n == 0)) diag("receiver's setresuid: errno=%d", errno);
		int got_before = int_got;
		n = sigprocmask(SIG_SETMASK, &oldset, NULL);
		fail_unless(n == 0);
		usleep(15 * 1000);	/* A_SHORT_NAP and a half */
		int got_after = int_got;
		const bool signaled = got_before < got_after;
		imply_ok1(!signaled, !from_root);
	} fork_subtest_end;

	if(!from_root) {
		n = setresuid(change_real ? 1111 : 1000,
			change_eff ? 1111 : 1000, change_saved ? 1111 : 1000);
		fail_unless(n == 0);
	}
	usleep(10 * 1000);	/* A_SHORT_NAP circa 2021 */
	n = kill(receiver, SIGINT);
	if(!ok1(n == 0 || (n < 0 && errno == EPERM))) {
		diag("errno=%d", errno);
	}
	ok1(kill(receiver, 0) == n);	/* carpet matches the drapes */
	fork_subtest_ok1(receiver);

	const bool signaled = (n == 0);
	imply_ok1(!signaled, !from_root);
	iff_ok1(signaled, from_root
		|| ((!change_real || !change_eff) && (!rc_real || !rc_saved)));
}
END_TEST

DECLARE_TEST("process:signal", kill_permissions);


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
	struct sigaction act = { .sa_handler = &count_sigint_handler };
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


/* test that the blocked signal mask is inherited across fork(), so that the
 * parent's mask guards children from receiving some signals until ready.
 */
START_LOOP_TEST(procmask_and_fork, iter, 0, 1)
{
	const bool unblock_sigint = !!(iter & 1);
	diag("unblock_sigint=%s", btos(unblock_sigint));
	plan_tests(2);

	int_got = 0;
	struct sigaction act = { .sa_handler = &count_sigint_handler };
	int n = sigaction(SIGINT, &act, NULL);
	fail_unless(n == 0);

	/* block SIGINT. */
	sigset_t sigint_set, oldset;
	sigemptyset(&sigint_set);
	if(!unblock_sigint) sigaddset(&sigint_set, SIGINT);
	n = sigprocmask(SIG_BLOCK, &sigint_set, &oldset);
	fail_unless(n == 0);
	ok1(!sigismember(&oldset, SIGINT));

	int child = fork_subtest_start("child sigprocmask(2)") {
		plan_tests(5);
		int first_got = int_got;
		usleep(5 * 1000);
		int second_got = int_got;
		n = sigprocmask(SIG_SETMASK, &oldset, NULL);
		usleep(5 * 1000);
		if(!ok1(n == 0)) diag("child sigprocmask failed, errno=%d", errno);
		ok(int_got > 0, "got signal");	/* move zig */
		imply_ok(!unblock_sigint, first_got == second_got,
			"when blocked, signaled between fork and unmask");
		imply_ok(!unblock_sigint, int_got > second_got,
			"when blocked, signaled after unmask");
		imply_ok(unblock_sigint, int_got == second_got,
			"when not blocked, signaled before unmask");
	} fork_subtest_end;

	n = sigprocmask(SIG_SETMASK, &oldset, NULL);
	fail_unless(n == 0);

	n = kill(child, SIGINT);
	fail_unless(n == 0);

	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("process:signal", procmask_and_fork);


/* sigsuspend. this could move off into process/sigsuspend.c or some such,
 * since sigsuspend (and the associated sigwait family) seems subject of
 * multiple tests by itself.
 */
START_LOOP_TEST(sigsuspend_basic, iter, 0, 1)
{
	const bool do_sleep = !!(iter & 1);
	diag("do_sleep=%s", btos(do_sleep));
	plan_tests(4);

	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	int n = sigprocmask(SIG_BLOCK, &block, &old);
	fail_if(n != 0);

	struct sigaction act = { .sa_handler = &count_sigint_handler };
	n = sigaction(SIGINT, &act, NULL);
	fail_if(n != 0);

	int_got = 0;
	int parent = getpid();
	int child = fork_subtest_start("signal-sending child") {
		plan_tests(1);
		n = kill(parent, SIGINT);
		if(!ok1(n == 0)) diag("errno=%d", errno);
	} fork_subtest_end;

	if(do_sleep) usleep(50 * 1000);
	ok(int_got == 0, "not signaled before suspend");
	do {
		n = sigsuspend(&old);
		if(n < 0 && errno != EINTR) break;
	} while(int_got == 0);
	ok1(n < 0 && errno == EINTR);
	ok(int_got > 0, "signaled during suspend");

	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("process:signal", sigsuspend_basic);


/* the Linux/glibc manpage suggests that sigsuspend(2) returns after exactly
 * one signal has caused a handler to run. test this by mostly the same as
 * above, but sending both SIGINT and SIGUSR1.
 */
START_LOOP_TEST(sigsuspend_multiple, iter, 0, 1)
{
	const bool do_sleep = !!(iter & 1);
	diag("do_sleep=%s", btos(do_sleep));
	plan_tests(6);

	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGUSR1);

	int n = sigprocmask(SIG_BLOCK, &block, &old);
	fail_if(n != 0);

	int_got = 0;
	handler_calls = 0;

	struct sigaction act = { .sa_handler = &count_sigint_handler };
	n = sigaction(SIGINT, &act, NULL);
	fail_if(n != 0);
	n = sigaction(SIGUSR1, &act, NULL);
	fail_if(n != 0);

	int parent = getpid();
	int child = fork_subtest_start("signal-sending child") {
		plan_tests(2);
		n = kill(parent, SIGINT);
		if(!ok(n == 0, "send SIGINT")) diag("errno=%d", errno);
		n = kill(parent, SIGUSR1);
		if(!ok(n == 0, "send SIGUSR1")) diag("errno=%d", errno);
	} fork_subtest_end;

	if(do_sleep) usleep(50 * 1000);
	ok(handler_calls == 0, "not signaled before suspend");
	int iters = 0;
	do {
		iters++;
		n = sigsuspend(&old);
		if(n < 0 && errno != EINTR) break;
	} while(handler_calls < 2);
	ok(n < 0 && errno == EINTR, "sigsuspend(2)");
	ok(int_got == 1, "one SIGINT handled");
	ok(handler_calls == 2, "two signals handled");
	ok(iters == 2, "sigsuspend called twice");

	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("process:signal", sigsuspend_multiple);


static volatile sig_atomic_t handler_ran = 0;

static void errno_smashing_handler_fn(int signum)
{
	handler_ran = 1;
	/* hypothetically, if writing errno in userspace were considered not
	 * signal-safe, a host might not restore errno unless it was overwritten
	 * in a signal handler by an errno producer, so manually assigning it
	 * would leave the existing value smashed. just for that case we'll have
	 * a library call smash it for us.
	 */
	struct sigaction act = { .sa_handler = &errno_smashing_handler_fn };
	int n = sigaction(123, &act, NULL);
	if(n == 0 || errno != EINVAL) {
		diag("%s: unexpected sigaction n=%d, errno=%d", __func__, n, errno);
	}
}


/* test that errno is preserved when signals occur over a function that's not
 * specified to set EINTR when signaled (i.e. sched_yield(2)). this test is
 * structurally very similar to the l4x2_vregs_preservation test, the major
 * difference being that this one has the portable bits and the other has
 * __l4x2__ specific preserved bits.
 *
 * variables:
 *   - async or sync signaling, i.e. from a child process into a
 *     while-sched_yield loop, or mask-raise-unmask.
 */
START_LOOP_TEST(errno_preservation, iter, 0, 1)
{
	const bool do_async = !!(iter & 1);
	diag("do_async=%s", btos(do_async));
	plan_tests(2);

	struct sigaction act = {
		.sa_handler = &errno_smashing_handler_fn,
	};
	int n = sigaction(SIGUSR1, &act, NULL);
	fail_if(n != 0, "sigaction failed, errno=%d", errno);

	/* TODO: move this, and the similar block in l4x2_vregs_preservation, into
	 * a common function returning `child' & filling in `oldmask'.
	 */
	sigset_t oldmask;
	int child = -1;
	if(do_async) {
		int parent = getpid();
		child = fork_subtest_start("signal sender") {
			plan(1);
			/* clumsy spin-based handshake necessitated by sigsuspend()'s
			 * propensity to also smash errno
			 */
			usleep(10 * 1000);
			n = kill(parent, SIGUSR1);
			if(!ok(n == 0, "kill(parent, SIGUSR1)")) diag("errno=%d", errno);
		} fork_subtest_end;
	} else {
		sigset_t block;
		sigemptyset(&block);
		sigaddset(&block, SIGUSR1);
		n = sigprocmask(SIG_BLOCK, &block, &oldmask);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);
		raise(SIGUSR1);
	}

	diag("waiting for handler...");
	errno = 12345;

	if(!do_async) {
		n = sigprocmask(SIG_SETMASK, &oldmask, NULL);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);
	}

	while(handler_ran == 0) sched_yield();

	if(!ok(errno == 12345, "errno was preserved")) {
		diag("abnormal errno=%d", errno);
	}

	if(do_async) fork_subtest_ok1(child);
	else skip(1, "no subtest in this iteration");
}
END_TEST

DECLARE_TEST("process:signal", errno_preservation);


#ifdef __l4x2__
static void l4x2_vregs_smashing_fn(int signum)
{
	n_sigs_handled++;

	L4_ThreadId_t dne = L4_GlobalId(L4_ThreadNo(L4_Myself()),
		(L4_Version(L4_Myself()) ^ 0x3f) | signum);
	assert(!L4_IsLocalId(dne));

	L4_Set_VirtualSender(dne);
	L4_Set_XferTimeouts(L4_Timeouts(L4_ZeroTime, L4_Never));
	/* TODO: change CopFlags, PreemptFlags as well */
	L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD0) = 0xdeadbeef;
	L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD1) = 0xc0def007 ^ signum;

	/* alter ErrorCode as well. we have two modes: one for generating ec=2
	 * (invalid tid to ExchangeRegisters), another for ec=4 (non-existing
	 * partner in Ipc send phase).
	 */
	L4_Word_t old_ec = L4_ErrorCode();
	if(old_ec != 2) {
		L4_ThreadId_t nil = L4_LocalIdOf(dne);
		assert(L4_IsNilThread(nil));
		assert(L4_ErrorCode() == 2);
	} else {
		L4_ThreadId_t dummy;
		L4_Ipc(dne, L4_nilthread,
			L4_Timeouts(L4_ZeroTime, L4_ZeroTime), &dummy);
		assert(L4_ErrorCode() == 4);
	}
	assert(L4_ErrorCode() != old_ec);
}
#endif


/* whether L4.X2 vregs are preserved in such a way that doesn't forbid IPC
 * setup and decoding in non-annotated sections of userspace.
 *
 * variables:
 *   - whether signal is processed asynchronously (from child, during
 *     sched_yield() loop), or synchronously (mask-raise-unmask).
 */
START_LOOP_TEST(l4x2_vregs_preservation, iter, 0, 1)
{
#ifndef __l4x2__
	diag("iter=%d", iter);	/* touch-a my spaghetti */
	plan(SKIP_ALL, "not applicable to non-L4.X2 platforms");
	done_testing();
#else
	const bool do_async = !!(iter & 1);
	diag("do_async=%s", btos(do_async));
	plan_tests(6);

	struct sigaction act = { .sa_handler = &l4x2_vregs_smashing_fn };
	int n = sigaction(SIGUSR1, &act, NULL);
	if(n != 0) BAIL_OUT("sigaction failed, errno=%d", errno);

	sigset_t oldmask;
	int child = -1;
	if(do_async) {
		int parent = getpid();
		child = fork_subtest_start("signal sender") {
			plan(1);
			/* clumsy spin-based handshake necessitated by sigsuspend()'s
			 * propensity to also smash MRs and named vregs
			 */
			usleep(10 * 1000);
			n = kill(parent, SIGUSR1);
			if(!ok(n == 0, "kill(parent, SIGUSR1)")) diag("errno=%d", errno);
		} fork_subtest_end;
	} else {
		sigset_t block;
		sigemptyset(&block);
		sigaddset(&block, SIGUSR1);
		n = sigprocmask(SIG_BLOCK, &block, &oldmask);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);
		raise(SIGUSR1);
	}

	assert(n_sigs_handled == 0);
	L4_Set_VirtualSender(L4_Myself());
	assert(L4_SameThreads(L4_ActualSender(), L4_Myself()));
	L4_Word_t tos = L4_Timeouts(L4_TimePeriod(12345), L4_TimePeriod(66642));
	L4_Set_XferTimeouts(tos);
	L4_Word_t old_ec = L4_ErrorCode();
	L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD0) = 0x70a57b07;
	L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD1) = 0xaa60a75e;

	if(!do_async) {
		n = sigprocmask(SIG_SETMASK, &oldmask, NULL);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);
	}

	while(n_sigs_handled == 0) sched_yield();

	ok(L4_ActualSender().raw == L4_Myself().raw, "vs/as");
	ok(old_ec == L4_ErrorCode(), "ec");
	ok(L4_XferTimeouts() == tos, "xfertos");
	ok(L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD0) == 0x70a57b07,
		"threadword0");
	ok(L4_VREG(__L4_Get_UtcbAddress(), L4_TCR_THREADWORD1) == 0xaa60a75e,
		"threadword1");

	if(do_async) fork_subtest_ok1(child);
	else skip(1, "no subtest in this iteration");
#endif
}
END_TEST

DECLARE_TEST("process:signal", l4x2_vregs_preservation);


static jmp_buf longjmp_target;

static void longjmp_once_fn(int signum) {
	diag("%s: signum=%d", __func__, signum);
	if(++n_sigs_handled == 1) longjmp(longjmp_target, signum);
}


/* arrange for simultaneous delivery of multiple signals. longjmp out of the
 * first handler. result should be that 1) setjmp returns with longjmp'd value
 * (signal number of first to handler), and 2) handler is run for both
 * signals.
 *
 * this is why we can't have nice things.
 *
 * variables:
 *   - [from_child] send signals from a child process, or locally
 *   - TODO: [jmp_on_second] longjmp() on first/second signal
 */
START_LOOP_TEST(longjmp_interference, iter, 0, 1)
{
	const bool from_child = !!(iter & 1);
	diag("from_child=%s", btos(from_child));
	plan_tests(3);

	int value;
	volatile int jmp_got = -1, n_jmps = 0;
	if((value = setjmp(longjmp_target)) != 0) {
		diag("setjmp returned %d", value);
		if(++n_jmps == 1) jmp_got = value;
	} else {
		sigset_t pair, oldset;
		sigemptyset(&pair);
		sigaddset(&pair, SIGUSR1);
		sigaddset(&pair, SIGUSR2);
		int n = sigprocmask(SIG_BLOCK, &pair, &oldset);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);

		struct sigaction act = { .sa_handler = &longjmp_once_fn };
		n = sigaction(SIGUSR1, &act, NULL);
		if(n == 0) n = sigaction(SIGUSR2, &act, NULL);
		if(n != 0) BAIL_OUT("sigaction failed, errno=%d", errno);

		if(from_child) {
			int parent = getpid();
			int child = fork();
			if(child == 0) {
				kill(parent, SIGUSR1);
				kill(parent, SIGUSR2);
				exit(0);
			}
			int st, dead = waitpid(child, &st, 0);
			if(dead != child || !WIFEXITED(st)) BAIL_OUT("child fuckup");
		} else {
			raise(SIGUSR1);
			raise(SIGUSR2);
		}

		/* thar she blows!! */
		n = sigprocmask(SIG_SETMASK, &oldset, NULL);
		if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);
		pause();
		BAIL_OUT("should not be reached!");
	}

	ok1(n_sigs_handled == 2);
	ok1(n_jmps == 1);
	ok1(jmp_got == SIGUSR1 || jmp_got == SIGUSR2);
}
END_TEST

DECLARE_TEST("process:signal", longjmp_interference);


static void squashing_handler_fn(int signum) {
	sig_got = signum;
	n_sigs_handled++;
}

/* test the squashing behaviour of reliable and realtime signals.
 *
 * variables:
 *   - [from_child] raise, or kill from child
 *   - [dupe] different/same signal
 *   - [realtime] reliable/realtime
 */
START_LOOP_TEST(squashing, iter, 0, 7)
{
	const bool from_child = !!(iter & 1), dupe = !!(iter & 2),
		realtime = !!(iter & 4);
	diag("from_child=%s, dupe=%s, realtime=%s",
		btos(from_child), btos(dupe), btos(realtime));
	plan_tests(2);
#ifdef __sneks__
	if(dupe && realtime) todo_start("pending signal queuing in UAPI");
#endif

	int sig1 = realtime ? SIGRTMIN+0 : SIGUSR1,
		sig2 = dupe ? sig1 : (realtime ? SIGRTMIN+1 : SIGUSR2);
	diag("sig1=%d, sig2=%d", sig1, sig2);

	sigset_t pair, oldset;
	sigemptyset(&pair);
	sigaddset(&pair, sig1);
	sigaddset(&pair, sig2);
	int n = sigprocmask(SIG_BLOCK, &pair, &oldset);
	if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);

	struct sigaction act = { .sa_handler = &squashing_handler_fn };
	n = sigaction(sig1, &act, NULL);
	if(n == 0 && sig1 != sig2) n = sigaction(sig2, &act, NULL);
	if(n != 0) BAIL_OUT("sigaction failed, errno=%d", errno);

	if(from_child) {
		int parent = getpid();
		int child = fork();
		if(child == 0) {
			kill(parent, sig1);
			kill(parent, sig2);
			exit(0);
		}
		int st, dead = waitpid(child, &st, 0);
		if(dead != child || !WIFEXITED(st)) BAIL_OUT("child fuckup");
	} else {
		raise(sig1);
		raise(sig2);
	}

	/* thar she blows!! */
	n = sigprocmask(SIG_SETMASK, &oldset, NULL);
	if(n != 0) BAIL_OUT("sigprocmask failed, errno=%d", errno);

	atomic_signal_fence(memory_order_acq_rel);
	iff_ok1(n_sigs_handled == 2, !dupe || realtime);
	ok1(sig_got == sig1 || sig_got == sig2);
}
END_TEST

DECLARE_TEST("process:signal", squashing);


#ifdef __l4x2__
static _Atomic int recv_sleeping_err;

static void recv_sleeping_handler_fn(int sig)
{
	L4_ThreadId_t dummy;
	L4_MsgTag_t tag = L4_Ipc(L4_nilthread, L4_MyLocalId(),
		L4_Timeouts(L4_ZeroTime, L4_TimePeriod(20 * 1000)), &dummy);
	atomic_store(&recv_sleeping_err,
		L4_IpcSucceeded(tag) ? 0 : L4_ErrorCode());
}
#endif


/* L4.X2 only: whether the signal handler for an interrupted receive phase,
 * such as due to __{permit,forbid}_recv_interrupt(), forbids receive
 * interrupts until re-enabled within handler context. (it should.)
 */
START_LOOP_TEST(forbid_recv_interrupt_in_handler, iter, 0, 1)
{
#ifndef __l4x2__
	plan_skip_all("not applicable outside L4.X2 (iter=%d)", iter);
#else
	/* FIXME: declare these in a header that's accessible */
	extern void __forbid_recv_interrupt(void);
	extern void __permit_recv_interrupt(void);

	const bool do_permit = !!(iter & 1);
	diag("do_permit=%s", btos(do_permit));
	plan_tests(7);

	struct sigaction act = { .sa_handler = &recv_sleeping_handler_fn };
	int n = sigaction(SIGUSR1, &act, NULL);
	if(!ok(n == 0, "sigaction[USR1]")) diag("n=%d, errno=%d", n, errno);
	struct sigaction off = { .sa_handler = &ignore_signal };
	n = sigaction(SIGUSR2, &off, NULL);
	if(!ok(n == 0, "sigaction[USR2]")) diag("n=%d, errno=%d", n, errno);

	atomic_store(&recv_sleeping_err, 0);

	int parent = getpid();
	L4_ThreadId_t parent_tid = L4_Myself(), child_tid;
	int child = fork_subtest_start("signal sender") {
		plan_tests(6);

		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Send(parent_tid);
		ok(L4_IpcSucceeded(tag), "parent introduction");

		/* sync to atomically put parent in receive stage */
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_Receive_Timeout(parent_tid, L4_TimePeriod(20 * 1000));
		ok(L4_IpcSucceeded(tag), "parent synced");
		int n = kill(parent, SIGUSR1);
		if(!ok(n == 0, "kill[USR1]")) diag("errno=%d", errno);

		usleep(8 * 1000);	/* slightly weaker a sync */
		n = kill(parent, SIGUSR2);
		if(!ok(n == 0, "kill[USR2]")) diag("errno=%d", errno);

		L4_LoadMR(0, 0);
		tag = L4_Reply(parent_tid);
		L4_Word_t ec = L4_IpcFailed(tag) ? L4_ErrorCode() : 0;
		imply_ok1(do_permit, ec == 2);
		imply_ok1(!do_permit, ec == 0);
		diag("ec=%lu", ec);
	} fork_subtest_end;

	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Wait(&child_tid);
	ok(L4_IpcSucceeded(tag) && pidof_NP(child_tid) == child,
		"got helper tid");

	if(do_permit) __permit_recv_interrupt();
	L4_LoadMR(0, 0);
	tag = L4_Call(child_tid);
	if(do_permit) __forbid_recv_interrupt();
	iff_ok1(!do_permit, L4_IpcSucceeded(tag));
	imply_ok1(do_permit, L4_ErrorCode() == 7);
	diag("fail=%s, ec=%lu", btos(L4_IpcFailed(tag)), L4_ErrorCode());
	fork_subtest_ok1(child);
	if(!ok(recv_sleeping_err == 3, "signal handler's receive timed out")) {
		diag("recv_sleeping_err=%d", recv_sleeping_err);
	}
#endif
}
END_TEST

DECLARE_TEST("process:signal", forbid_recv_interrupt_in_handler);
