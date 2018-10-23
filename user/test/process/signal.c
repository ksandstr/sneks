
/* tests on POSIX signaling: sigaction, kill, and so forth. */

#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/syscall.h>

#include <sneks/test.h>


static sig_atomic_t chld_got = 0;


static void sigchld_handler(int signum)
{
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
	plan_tests(3);

	chld_got = 0;
	struct sigaction act = { .sa_handler = &sigchld_handler };
	int n = sigaction(SIGCHLD, &act, NULL);
	if(!ok(n == 0, "sigaction")) diag("errno=%d", errno);
	int child = fork();
	if(child == 0) {
		/* ensure that the parent gets to a sleep.
		 *
		 * TODO: the no-sleep case is contained in the uninterruptible receive
		 * case, but should be provoked explicitly as well.
		 */
		L4_Sleep(L4_TimePeriod(2 * 1000));
		exit(0);
	}
	if(!ok(child > 0, "fork")) diag("errno=%d", errno);

	const L4_Time_t iter_timeout = L4_TimePeriod(5 * 1000);
	int iters = 5;
	while(child > 0 && chld_got < 1 && --iters) {
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
	}
	if(!ok(iters > 0, "signal was processed") && child > 0) {
		int st, dead = wait(&st);
		diag("waited for dead=%d (child=%d)", dead, child);
	}
}
END_TEST

DECLARE_TEST("process:signal", sigaction_basic);



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

	L4_Sleep(L4_TimePeriod(5 * 1000));	/* FIXME: use posix sleep instead */
	int st, dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead < 0 && errno == ECHILD, "no waitpid before signal")) {
		diag("dead=%d, st=%d, errno=%d", dead, st, errno);
	}

	int n = kill(child, SIGCHLD);	/* FIXME: use something different */
	if(n != 0) diag("kill(2) failed, errno=%d", errno);

	L4_Sleep(L4_TimePeriod(5 * 1000));
	dead = waitpid(-1, &st, WNOHANG);
	if(!ok(dead == child, "waitpid succeeds after signal")) {
		diag("dead=%d, st=%d, errno=%d", dead, st, errno);
		/* FIXME: clean the child up with SIGKILL or something. */
	}
}
END_TEST

DECLARE_TEST("process:signal", pause_basic);
