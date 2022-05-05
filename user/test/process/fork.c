#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <sneks/test.h>
#ifdef __l4x2__
#include <l4/types.h>
#include <l4/ipc.h>
#endif

START_LOOP_TEST(fork_basic, iter, 0, 1)
{
#ifdef __l4x2__
	const bool active_exit = iter & 1;
	diag("active_exit=%s", btos(active_exit));
	plan_tests(3);

	L4_ThreadId_t parent = L4_Myself();
	int child = fork();
	if(child == 0) {
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
		L4_LoadMR(1, 666);	/* number of the beast */
		L4_MsgTag_t tag = L4_Send_Timeout(parent, L4_TimePeriod(10 * 1000));
		if(active_exit) L4_Sleep(L4_TimePeriod(10 * 1000));
		exit(L4_IpcSucceeded(tag) ? 0 : 1);
	}

	skip_start(!ok1(child > 0), 1, "child=%d", child) {
		L4_ThreadId_t sender;
		L4_MsgTag_t tag = L4_Wait_Timeout(L4_TimePeriod(50 * 1000), &sender);
		L4_Word_t value; L4_StoreMR(1, &value);
		if(!ok(L4_IpcSucceeded(tag) && value == 666, "correct ipc from child")) diag("ec=%lu", L4_ErrorCode());
	} skip_end;

	if(!active_exit) L4_Sleep(L4_TimePeriod(10 * 1000));
	int st, pid = wait(&st);
	ok(pid > 0 && pid == child, "child was waited on");
#else
	plan(SKIP_ALL, "requires L4.X2 Ipc",
		iter /* this shuts up the compiler */);
#endif
}
END_TEST

DECLARE_TEST("process:fork", fork_basic);

/* tests that it's possible to fork a bunch of times concurrently. */
START_LOOP_TEST(fork_wide, iter, 0, 1)
{
	const int n_children = iter & 1 ? 48 : 8;
	diag("n_children=%d", n_children);
	plan_tests(3);

	int children[n_children], n_started = 0, n_waited = 0, n, st;
	for(int i=0; i < n_children; i++) {
		if(children[i] = fork(), children[i] > 0) n_started++;
		else {
			fail_unless(children[i] == 0, "fork failed, errno=%d", errno);
			exit(0);
		}
	}
	if(!ok1(n_started == n_children)) diag("n_started=%d", n_started);
	do if(n = wait(&st), n > 0) n_waited++; while(n > 0);
	if(!ok1(n < 0 && errno == ECHILD)) diag("n=%d, errno=%d", n, errno);
	if(!ok1(n_waited == n_children)) diag("n_waited=%d", n_waited);
}
END_TEST

DECLARE_TEST("process:fork", fork_wide);

/* tests recursive forking. */
START_LOOP_TEST(fork_deep, iter, 0, 1)
{
	const int depth = ~iter & 1 ? 2 : 20;
	diag("depth=%d", depth);
	plan_tests(2);

	int child = -1, level = 0;
	while(level <= depth) {
		child = fork();
		if(child > 0) break;
		level++;
	}
	if(child == 0) {
		diag("deepest child pid=%d exiting", getpid());
		exit(0);
	}
	int st, pid = wait(&st);
	if(level > 0) {
		diag("level=%d, child=%d, self=%d", level, child, getpid());
		exit(pid == child && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1);
	}
	ok1(pid == child);
	ok1(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
END_TEST

DECLARE_TEST("process:fork", fork_deep);

/* access memory that wasn't mapped into the parent. this should pop a failure
 * in vm that only forks pages but not the maps they came from.
 */
START_TEST(access_mmap_memory)
{
	plan_tests(1);
	static char block[16 * 1024];
	memset(block, 0, sizeof block);
	pass("didn't segfault");
}
END_TEST

DECLARE_TEST("process:fork", access_mmap_memory);

/* forking within a signal handler is one of those ``through the looking
 * glass'' things of POSIX.
 *
 * in sneks, calling fork() within a signal handler and not exiting either the
 * child-half or parent-half will cause one of two things to happen, depending
 * on whether the signal came from within the process or from without: a
 * caller of raise(3) will return twice, and an external source will
 * effectively cause the receiver to fork without warning. this is because
 * signal invocation always happens onto a valid stack frame sequence even
 * when signals are handled during handler execution; when that sequence is
 * duplicated by fork(), the rest of the handler chain executes as expected
 * down either parent, child, or both.
 *
 * to test this behaviour we'll do both here: fork-to-exit in response to the
 * first 5 SIGCHLD, and fork-to-return in response to SIGINT which starts the
 * sequence off.
 */
static int last_fork_child = 0;
static bool fork_first_st_ok = true, fork_later_st_ok = true;
static volatile sig_atomic_t last_child_signaled = 0;

static void forking_handler(int signum)
{
	static int n_forks = 0;
	if(signum == SIGINT) {
		last_fork_child = fork();
		/* invisible steering wheel! */
	} else if(signum == SIGCHLD && n_forks++ < 5) {
		int st, dead = waitpid(-1, &st, 0);
		if(dead < 0) {
			diag("%s: dead=%d, errno=%d", __func__, dead, errno);
		}
		int n = fork();
		if(n == 0) {
			/* wasn't long for this world. oh well */
			exit(0);
		} else {
			last_fork_child = n;
			fork_first_st_ok = fork_first_st_ok && (n_forks > 1 || WEXITSTATUS(st) != 0);
			fork_later_st_ok = fork_later_st_ok && (n_forks == 1 || WEXITSTATUS(st) == 0);
		}
	} else {
		last_child_signaled = 1;
	}
}

/* see comment above. doing any of this in production code may lead to
 * surprisingly unorthodox control flow and (more importantly) is very
 * naughty, so don't do it lest the local tailor have your thumbs off.
 */
START_TEST(from_signal_handler)
{
	plan_tests(6);

	sigset_t chld_set, old;
	sigemptyset(&chld_set);
	sigaddset(&chld_set, SIGCHLD);
	int n = sigprocmask(SIG_BLOCK, &chld_set, &old);
	fail_if(n != 0, "errno=%d", errno);

	struct sigaction act = { .sa_handler = &forking_handler };
	sigaction(SIGCHLD, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	/* start the dance */
	int parent_pid = getpid();
	kill(getpid(), SIGINT);
	if(getpid() != parent_pid) {
		exit(0xb0a7);	/* confirm second return from kill(2) */
	}

	int sigint_child = last_fork_child;
	/* proceed on last SIGCHLD */
	while(!last_child_signaled) {
		n = sigsuspend(&old);
		if(n != -1 || errno != EINTR) {
			diag("sigsuspend returned n=%d, errno=%d", n, errno);
			break;
		}
	}

	/* wait for the last child */
	int st, dead = waitpid(-1, &st, 0);
	ok(dead > 0, "waitpid(2)");
	ok1(dead != sigint_child);
	ok1(dead == last_fork_child);
	ok1(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	ok(fork_first_st_ok, "first rc correct");
	ok(fork_later_st_ok, "later rc correct");
}
END_TEST

DECLARE_TEST("process:fork", from_signal_handler);
