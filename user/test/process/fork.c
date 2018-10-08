
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/wait.h>
#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/test.h>


START_LOOP_TEST(fork_basic, iter, 0, 1)
{
	const bool active_exit = (iter & 1) != 0;
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
		if(!ok(L4_IpcSucceeded(tag) && value == 666,
			"correct ipc from child"))
		{
			diag("ec=%#lu", L4_ErrorCode());
		}
	} skip_end;

	if(!active_exit) L4_Sleep(L4_TimePeriod(10 * 1000));
	int st, pid = wait(&st);
	ok(pid > 0 && pid == child, "child was waited on");
}
END_TEST

DECLARE_TEST("process:fork", fork_basic);


/* tests that it's possible to fork a bunch of times concurrently, without
 * crashing the system.
 */
START_LOOP_TEST(fork_wide, iter, 0, 1)
{
	const int n_children = (iter & 1) != 0 ? 64 : 8;
	diag("n_children=%d", n_children);
	plan_tests(3);

	int children[n_children], n_started = 0;
	for(int i=0; i < n_children; i++) {
		children[i] = fork();
		if(children[i] > 0) n_started++;
		else {
			assert(children[i] == 0);
			exit(0);
		}
	}
	if(!ok1(n_started == n_children)) {
		diag("n_started=%d", n_started);
	}

	int n_waited = 0, n;
	do {
		int st;
		n = wait(&st);
		if(n > 0) n_waited++;
	} while(n > 0);
	if(!ok1(n < 0 && errno == ECHILD)) diag("n=%d, errno=%d", n, errno);
	if(!ok1(n_waited == n_children)) diag("n_waited=%d", n_waited);
}
END_TEST

DECLARE_TEST("process:fork", fork_wide);
