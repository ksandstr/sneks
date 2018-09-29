
/* tests on C11 condition variables. these could and should be shared with
 * userspace, if not non-L4.X2 systems due to the IPC dependency.
 *
 * TODO: most tests here won't terminate unless cnd_signal and cnd_broadcast
 * actually do something. this is a known defect but not worth getting
 * involved with until the test harness supports test validation through break
 * patches or some such. for now these tests are effectively 1½-tailed, which
 * is better than less or nothing at all.
 */

#include <stdlib.h>
#include <threads.h>
#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/syscall.h>
#include <sneks/thread.h>
#include <sneks/test.h>


START_TEST(init_cnd)
{
	plan_tests(2);

	mtx_t *m = malloc(sizeof *m);
	mtx_init(m, mtx_plain);
	cnd_t *c = malloc(sizeof *c);
	int n = cnd_init(c);
	ok(n == thrd_success, "cnd_init()");

	cnd_destroy(c);
	pass("cnd_destroy() didn't crash");
	free(c);

	mtx_destroy(m);
	free(m);
}
END_TEST


struct sleep_pair {
	mtx_t m;
	cnd_t c, *cp;
	L4_ThreadId_t parent;
};


static int sleep_on_it(void *param_ptr)
{
	struct sleep_pair *p = param_ptr;
	mtx_lock(&p->m);
	L4_ThreadId_t parent = p->parent;
	int n = cnd_wait(p->cp, &p->m);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, n);
	L4_MsgTag_t tag = L4_Send(parent);
	mtx_unlock(&p->m);
	return L4_IpcSucceeded(tag) ? 0 : L4_ErrorCode();
}


/* tests that the helper thread reports back only after cnd_signal() was
 * called, and that its cnd_wait() didn't report an error.
 *
 * NOTE: this is a relatively weak test that fails to complete if cnd_wait()
 * doesn't wake up at all. TODO: there's likely some even simpler test for the
 * barest basics, but this ain't it.
 */
START_TEST(signal)
{
	plan_tests(4);

	struct sleep_pair *p = malloc(sizeof *p);
	p->parent = L4_Myself();
	mtx_init(&p->m, mtx_plain);
	cnd_init(&p->c);
	p->cp = &p->c;

	mtx_lock(&p->m);
	thrd_t t;
	int n = thrd_create(&t, &sleep_on_it, p);
	assert(n == thrd_success);
	mtx_unlock(&p->m);

	L4_MsgTag_t tag = L4_Receive_Timeout(thrd_to_tid(t),
		L4_TimePeriod(5 * 1000));
	ok(L4_IpcFailed(tag) && L4_ErrorCode() == 3,
		"before signal, receive times out");
	cnd_signal(&p->c);
	tag = L4_Receive_Timeout(thrd_to_tid(t), L4_TimePeriod(5 * 1000));
	L4_Word_t status; L4_StoreMR(1, &status);
	ok(L4_IpcSucceeded(tag), "after signal, receive completes");
	ok(status == thrd_success, "partner status is thrd_success");

	int res;
	n = thrd_join(t, &res);
	if(n != thrd_success) diag("thrd_join failed: n=%d", n);
	ok(n == thrd_success && res == 0, "partner IPC succeeded");

	mtx_destroy(&p->m);
	cnd_destroy(&p->c);
	free(p);
}
END_TEST


/* similar to the single-thread signal test, but with many threads and using
 * cnd_broadcast().
 *
 * TODO: this won't terminate if both cnd_signal() and cnd_broadcast() are
 * stubs.
 */
START_TEST(broadcast)
{
	const int n_threads = 8;
	diag("n_threads=%d", n_threads);

	plan_tests(4);

	thrd_t ts[n_threads];
	struct sleep_pair *ps[n_threads];
	cnd_t *cond = malloc(sizeof *cond);
	cnd_init(cond);
	int n_valid = 0;
	for(int i=0; i < n_threads; i++) {
		ps[i] = malloc(sizeof *ps[i]);
		ps[i]->parent = L4_MyLocalId();
		mtx_init(&ps[i]->m, mtx_plain);
		ps[i]->cp = cond;
		int n = thrd_create(&ts[i], &sleep_on_it, ps[i]);
		if(n != thrd_success) {
			diag("failed to create thread for i=%d: n=%d", i, n);
			ps[i]->parent = L4_nilthread;	/* invalid ts[i] */
		} else {
			n_valid++;
		}
	}

	L4_ThreadId_t from;
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_TimePeriod(10 * 1000), &from);
	ok(L4_IpcFailed(tag) && L4_ErrorCode() == 3,
		"timed out before broadcast");

	int n = cnd_broadcast(cond);
	if(!ok(n == thrd_success, "cnd_broadcast()")) {
		/* do the same thing manually. */
		for(int i=0; i < n_threads; i++) cnd_signal(cond);
	}

	int n_got = 0;
	while(n_got < n_threads) {
		tag = L4_WaitLocal_Timeout(L4_TimePeriod(10 * 1000), &from);
		if(L4_IpcFailed(tag)) {
			diag("receive failed, ec=%lu", L4_ErrorCode());
			break;
		}
		L4_Word_t status; L4_StoreMR(1, &status);
		if(!L4_IsLocalId(from)) diag("from=%#lx wasn't local?", from.raw);
		if(status != 0) {
			diag("from=%#lx status=%lu (not 0)?", from.raw, status);
		}
		for(int i=0; i < n_threads; i++) {
			if(!L4_IsNilThread(ps[i]->parent)
				&& ps[i]->cp != NULL
				&& L4_SameThreads(thrd_to_tid(ts[i]), from))
			{
				ps[i]->cp = NULL;
				n_got++;
			}
		}
	}
	if(!ok(n_got == n_threads, "got status from all threads")) {
		diag("n_got=%d", n_got);
	}

	bool joins_ok = true;
	for(int i=0; i < n_threads; i++) {
		if(L4_IsNilThread(ps[i]->parent)) continue;
		int status;
		n = thrd_join(ts[i], &status);
		if(n != thrd_success) {
			diag("failed to join at i=%d: n=%d", i, n);
			joins_ok = false;
		}
	}
	ok1(joins_ok);
}
END_TEST


SYSTEST("crt:cnd", init_cnd);
SYSTEST("crt:cnd", signal);
SYSTEST("crt:cnd", broadcast);
